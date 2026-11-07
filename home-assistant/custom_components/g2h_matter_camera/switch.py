"""Verified camera audio and radio controls on the existing Matter node."""
import asyncio
from contextlib import suppress
from datetime import timedelta
import logging

from aiohttp import ClientError
from homeassistant.components.matter.helpers import get_matter, get_node_device_identifier
from homeassistant.components.switch import SwitchEntity
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity, DataUpdateCoordinator, UpdateFailed
from matter_server.client.exceptions import MatterClientException
from matter_server.common.errors import MatterError

from . import DOMAIN

_LOGGER = logging.getLogger(__name__)
AUDIO_PATH = f'1/{0xFFF1FC11}/3'
THREAD_PATH = f'0/{0xFFF1FC10}/'
MODE_REVISION = {'audio': '1/80/65533', 'thread': '0/80/65533'}
PATHS = [AUDIO_PATH] + [THREAD_PATH + str(i) for i in (24, 25, 26, 27)] + list(MODE_REVISION.values())
NAMES = {'audio': 'Geluid', 'thread': 'Thread'}
VIDEO_REVISION='2/80/65533'
VIDEO_STATE='2/80/3'
NAMES['video']='Video'


def control_values(values):
    """Only an observed Boolean is a state; requests/null/transitions are not."""
    if not isinstance(values, dict):
        raise ValueError('Invalid camera control response')
    audio = values.get(AUDIO_PATH)
    requested, observed = (values.get(THREAD_PATH + str(i)) for i in (24, 25))
    pending, error = (values.get(THREAD_PATH + str(i)) for i in (26, 27))
    thread_supported = (values.get(MODE_REVISION['thread']) == 2
                        and all(THREAD_PATH + str(i) in values for i in (24, 25, 26, 27)))
    # A later, fresh physical observation remains authoritative even when the
    # last command failed. Otherwise a sticky error would strand an unavailable
    # switch and prevent retrying it from the UI. Never echo the requested state.
    thread_known = (thread_supported and type(observed) is bool
                    and (requested is None or type(requested) is bool)
                    and pending is False and error in (
                        '', 'uart-busy', 'uart-unavailable', 'uart-settings',
                        'command-failed', 'verification-failed'))
    return {
        'audio': {'supported': AUDIO_PATH in values and values.get(MODE_REVISION['audio']) == 2,
                  'state': audio if type(audio) is bool else None},
        'thread': {'supported': thread_supported, 'state': observed if thread_known else None,
                   'requested': requested if type(requested) is bool else None,
                   'pending': pending if type(pending) is bool else None,
                   'error': error if error in ('', 'uart-busy', 'uart-unavailable', 'uart-settings',
                       'command-failed', 'verification-failed') else 'unavailable'},
    }


class ControlsCoordinator(DataUpdateCoordinator):
    """One retained RPC prevents abandoned operations from forming a queue."""
    local_timeout = 5
    verification_timeout = 15
    verification_interval = .25

    def __init__(self, hass, entry):
        super().__init__(hass, _LOGGER, name='G2H camera controls', config_entry=entry,
                         update_interval=timedelta(seconds=10), always_update=False)
        self.entry = entry
        self.closed = False
        self._pending_rpc = None
        self._lock = asyncio.Lock()
        self.writing = set()
        self.failed = set()
        self._video_supported = None

    def _client(self):
        runtime = self.hass.data[DOMAIN][self.entry.entry_id]
        client = runtime['client']
        if get_matter(self.hass).matter_client is not client:
            raise ValueError('Existing Matter client changed')
        return client

    async def _rpc(self, operation):
        if self.closed:
            raise ValueError('Camera controls stopped')
        pending = self._pending_rpc
        if pending is not None:
            if not pending.done():
                raise TimeoutError('Previous Matter operation still pending')
            # Never use an old read as the observation of a new write.
            with suppress(asyncio.CancelledError, Exception):
                await pending
            self._pending_rpc = None

        async def bounded():
            async with asyncio.timeout(30):
                return await operation()

        pending = asyncio.create_task(bounded(), name='G2H single control operation')
        self._pending_rpc = pending
        try:
            return await asyncio.wait_for(asyncio.shield(pending), self.local_timeout)
        finally:
            if pending.done() and self._pending_rpc is pending:
                self._pending_rpc = None

    async def _read(self):
        client = self._client()
        values = await self._rpc(lambda: client.read_attribute(self.entry.data['node_id'], list(PATHS)))
        data=control_values(values)
        # Probe separately: an unsupported endpoint on older firmware must not
        # make the existing sound/radio controls disappear.
        if self._video_supported is None:
            try:
                revision=await self._rpc(lambda: client.read_attribute(self.entry.data['node_id'],[VIDEO_REVISION]))
                self._video_supported=isinstance(revision,dict) and revision.get(VIDEO_REVISION)==2
            except TimeoutError:
                raise
            except (ValueError,OSError,RuntimeError,MatterError,MatterClientException,ClientError):
                pass
        state=None
        if self._video_supported:
            try:
                value=await self._rpc(lambda: client.read_attribute(self.entry.data['node_id'],[VIDEO_STATE]))
                mode=value.get(VIDEO_STATE) if isinstance(value,dict) else None
                if type(mode) is int and mode in (0,1):state=mode==1
            except TimeoutError:
                raise
            except (ValueError,OSError,RuntimeError,MatterError,MatterClientException,ClientError):
                pass
        data['video']={'supported':self._video_supported is True,'state':state}
        return data

    async def _async_update_data(self):
        try:
            async with self._lock:
                data = await self._read()
                self.failed.difference_update(key for key, value in data.items() if value['state'] is not None)
                return data
        except (ValueError, OSError, TimeoutError, RuntimeError, IndexError, KeyError,
                MatterError, MatterClientException, ClientError) as err:
            raise UpdateFailed('Cannot verify G2H audio and Thread controls through Matter') from err

    async def set_enabled(self, key, enabled):
        if key not in NAMES or type(enabled) is not bool:
            raise HomeAssistantError('Ongeldige G2H-schakelaar')
        if self.closed or self._lock.locked() or self.writing:
            raise HomeAssistantError('G2H is nog bezig; wacht even en probeer opnieuw')
        data = self.data if isinstance(self.data, dict) else {}
        if not data.get(key, {}).get('supported'):
            raise HomeAssistantError('Deze G2H-firmware ondersteunt deze schakelaar nog niet')
        self.writing.add(key)
        self.failed.discard(key)
        self.async_update_listeners()
        try:
            async with self._lock:
                client = self._client()
                # Existing servers can read unknown vendor attributes but reject
                # writing an unknown cluster model. Invoke standard Mode Select
                # and keep the vendor observations as the independent readback.
                await self._rpc(lambda: client.send_command('device_command',
                    node_id=self.entry.data['node_id'], endpoint_id={'audio':1,'thread':0,'video':2}[key],
                    cluster_id=80, command_name='ChangeToMode', payload={'newMode': int(enabled)}))
                if key=='video':
                    # The authenticated request queues a full camera boot.
                    # Report transition/unknown until a later real observation;
                    # do not hold the shared control lock across a long reboot.
                    observed=dict(self.data)
                    observed['video']={'supported':True,'state':None}
                    self.async_set_updated_data(observed)
                    return
                deadline = self.hass.loop.time() + self.verification_timeout
                while True:
                    observed = await self._read()
                    self.async_set_updated_data(observed)
                    if observed[key]['state'] is enabled:
                        return
                    if self.hass.loop.time() >= deadline:
                        raise ValueError('Control did not reach verified requested state')
                    await asyncio.sleep(self.verification_interval)
        except (ValueError, OSError, TimeoutError, RuntimeError, IndexError, KeyError,
                MatterError, MatterClientException, ClientError) as err:
            self.failed.add(key)
            raise HomeAssistantError(
                f'{NAMES[key]} is niet bevestigd door de G2H. '
                'Wacht tot de status beschikbaar is en probeer opnieuw.') from err
        finally:
            self.writing.discard(key)
            self.async_update_listeners()

    async def close(self):
        self.closed = True
        await self.async_shutdown()
        pending = self._pending_rpc
        self._pending_rpc = None
        if pending is not None:
            if not pending.done():
                pending.cancel()
            with suppress(asyncio.CancelledError, Exception):
                await pending


async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = ControlsCoordinator(hass, entry)
    hass.data[DOMAIN][entry.entry_id]['controls'] = coordinator
    # Unsupported optional attributes must not break camera/sensor platforms.
    await coordinator.async_refresh()
    data = coordinator.data if isinstance(coordinator.data, dict) else {}
    async_add_entities([CameraControlSwitch(coordinator, entry, key)
                        for key in NAMES if data.get(key, {}).get('supported')])


class CameraControlSwitch(CoordinatorEntity, SwitchEntity):
    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, coordinator, entry, key):
        super().__init__(coordinator)
        self.key = key
        self._attr_name = NAMES[key]
        self._attr_icon = {'audio':'mdi:volume-high','thread':'mdi:network','video':'mdi:video'}[key]
        self._attr_unique_id = entry.data['unique_id'] + '-control-' + key
        client = coordinator.hass.data[DOMAIN][entry.entry_id]['client']
        self._attr_device_info = DeviceInfo(identifiers={
            get_node_device_identifier(client.server_info, entry.data['node_id'])})

    @property
    def available(self):
        return (super().available and not self.coordinator.closed
                and self.key not in self.coordinator.writing
                and self.key not in self.coordinator.failed and self.is_on is not None)

    @property
    def is_on(self):
        data = self.coordinator.data
        return data.get(self.key, {}).get('state') if isinstance(data, dict) else None

    @property
    def extra_state_attributes(self):
        if self.key=='video':return {'camera_restart_required':True}
        if self.key!='thread' or not isinstance(self.coordinator.data,dict):
            return None
        value=self.coordinator.data.get('thread',{})
        return {'requested_enabled':value.get('requested'),
                'command_pending':value.get('pending'),
                'last_command_error':value.get('error')}

    async def async_turn_on(self, **kwargs):
        await self.coordinator.set_enabled(self.key, True)

    async def async_turn_off(self, **kwargs):
        await self.coordinator.set_enabled(self.key, False)
