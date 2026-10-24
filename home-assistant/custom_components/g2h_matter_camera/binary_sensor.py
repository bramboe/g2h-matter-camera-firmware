"""Native held-motion signal, not occupancy, using the existing Matter node."""
import asyncio
from contextlib import suppress
from datetime import timedelta
import logging
from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.components.matter.helpers import get_node_device_identifier
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, CoordinatorEntity, UpdateFailed
from . import DOMAIN
from .diagnostics import MOTION_CLUSTER, motion_values, read_cluster
_LOGGER = logging.getLogger(__name__)
class MotionCoordinator(DataUpdateCoordinator):
    def __init__(self, hass, entry):
        super().__init__(hass, _LOGGER, name='G2H native held motion', config_entry=entry,
                         update_interval=timedelta(milliseconds=500), always_update=False)
        self.entry = entry
        self.closed = False
        self._received_at = None
        self._sample_age_ms = None
        self._window_ms = None
        self.expiry = None
        self._expiry_generation = 0
        self._precise_task = None
        self._precise_handle = None
    def _schedule_refresh(self):
        # HA 2026.10 rounds its standard coordinator schedule to whole seconds;
        # that can place a 500-ms interval in the past. Schedule relative to the
        # current monotonic time, and never queue a burst of overdue reads.
        self._async_unsub_refresh()
        # Parent refresh helpers can clear _unsub_refresh while a read starts;
        # retain ownership of our handle so an expiry cannot orphan a timer.
        if self._precise_handle is not None:
            self._precise_handle.cancel()
            self._precise_handle = None
        interval = self._update_interval_seconds
        if interval is None or self.closed or self._shutdown_requested:
            return
        if self.config_entry and self.config_entry.pref_disable_polling:
            return
        delay = self._retry_after if self._retry_after is not None else interval
        self._retry_after = None
        self._precise_handle = self.hass.loop.call_later(max(.5,delay),self._precise_timer_fired)
        self._unsub_refresh = self._precise_handle.cancel
    def _precise_timer_fired(self):
        self._precise_handle = None
        self._unsub_refresh = None
        if self.closed or self._shutdown_requested or self.hass.is_stopping:
            return
        if self.config_entry and self.config_entry.pref_disable_polling:
            return
        if self._precise_task is not None and not self._precise_task.done():
            # An expiry may reschedule while a read is in flight. Its completion
            # schedules the next poll; do not queue another behind its lock.
            return
        coroutine = self._handle_refresh_interval()
        if self.config_entry:
            self._precise_task = self.config_entry.async_create_background_task(
                self.hass,coroutine,name='G2H native held motion refresh',eager_start=True)
        else:
            self._precise_task = self.hass.async_create_background_task(
                coroutine,name='G2H native held motion refresh',eager_start=True)
    async def close(self):
        self.closed = True
        await self.async_shutdown()
    async def async_shutdown(self):
        self.closed = True
        self._async_unsub_refresh()
        if self._precise_handle is not None:
            self._precise_handle.cancel()
            self._precise_handle = None
        task = self._precise_task
        self._precise_task = None
        if task is not None and not task.done() and task is not asyncio.current_task():
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task
        if self.expiry is not None:
            self.expiry.cancel()
            self.expiry = None
        await super().async_shutdown()
    @property
    def fresh(self):
        if self.closed or not self.data or self.data.get('motion') is None or self._received_at is None:
            return False
        age, window = self._sample_age_ms, self._window_ms
        return age is not None and window is not None and age + (self.hass.loop.time()-self._received_at)*1000 < window
    def _expire(self):
        self.expiry = None
        if self.closed or not self.data or self.data.get('motion') is None or self.fresh:
            return
        # Change the meaningful state once. This also makes a later fresh
        # sample trigger recovery even when its boolean matches the old value.
        self._expiry_generation += 1
        values = dict(self.data)
        values['expiry_generation'] = self._expiry_generation
        values.update({'motion':None, 'producer_enabled':None, 'reason':'sample-expired'})
        self.async_set_updated_data(values)
    async def _async_update_data(self):
        if self.closed:
            raise UpdateFailed('Motion coordinator stopped')
        try:
            values = motion_values(await read_cluster(self.hass, self.entry, MOTION_CLUSTER, (0,1,2,3,5,6), timeout=2))
            self._received_at = self.hass.loop.time()
            self._sample_age_ms = values.pop('sample_age_ms')
            self._window_ms = values['freshness_window_ms']
            if values['motion'] is None and values['reason'] in ('stale','sample-expired'):
                values.update({'producer_enabled':None, 'reason':'sample-expired'})
            if self.expiry is not None:
                self.expiry.cancel()
                self.expiry = None
            if values['motion'] is not None:
                delay = max(0, (self._window_ms-self._sample_age_ms)/1000)
                # A slow/disconnected next read must not keep the old motion
                # state available beyond the radio's stated freshness window.
                self.expiry = self.hass.loop.call_later(delay, self._expire)
            # An expiry may publish unavailable while this read is in flight.
            # HA compares against its pre-read data, so retain this transition
            # token to ensure even an unchanged boolean republishes recovery.
            values['expiry_generation'] = self._expiry_generation
            return values
        except (ValueError, OSError, TimeoutError) as err:
            raise UpdateFailed('Cannot read native motion through Matter') from err
async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = MotionCoordinator(hass, entry)
    hass.data[DOMAIN][entry.entry_id]['motion'] = coordinator
    # Older firmware and disabled native detection still load the camera; the
    # motion entity stays unavailable instead of inventing a clear sample.
    await coordinator.async_refresh()
    async_add_entities([MotionSensor(coordinator, entry)])
class MotionSensor(CoordinatorEntity, BinarySensorEntity):
    _attr_has_entity_name = True
    _attr_name = 'Motion'
    _attr_device_class = BinarySensorDeviceClass.MOTION
    def __init__(self, coordinator, entry):
        super().__init__(coordinator)
        self._attr_unique_id = entry.data['unique_id'] + '-motion'
        client = coordinator.hass.data[DOMAIN][entry.entry_id]['client']
        self._attr_device_info = DeviceInfo(identifiers={get_node_device_identifier(client.server_info, entry.data['node_id'])})
    @property
    def available(self):
        return super().available and self.coordinator.fresh
    @property
    def is_on(self):
        return self.coordinator.data.get('motion') if self.coordinator.fresh else None
    @property
    def extra_state_attributes(self):
        if not self.coordinator.data:
            return {}
        return {key:self.coordinator.data.get(key) for key in
                ('producer_enabled','reason','freshness_window_ms')}
