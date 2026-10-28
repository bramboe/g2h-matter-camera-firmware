"""Thread diagnostics on the existing camera's Matter device and fabric."""
import asyncio
from contextlib import suppress
from datetime import timedelta
import logging
from homeassistant.components.sensor import SensorEntity
from homeassistant.components.matter.helpers import get_node_device_identifier
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, CoordinatorEntity, UpdateFailed
from homeassistant.const import EntityCategory
from . import DOMAIN
from .diagnostics import read_cluster
from .thread_telemetry import CLUSTER, PATH, parse
_LOGGER = logging.getLogger(__name__)
NAMES = {'role':'Thread-rol', 'channel':'Thread-kanaal', 'neighbors':'Thread neighbors',
         'children':'Attached Thread children', 'routers':'Known Thread routers',
         'tx_total':'Radio MAC transmitted packets', 'rx_total':'Radio MAC received packets',
         'tx_retry':'Radio MAC retries', 'uptime_ms':'Thread radio uptime'}
class RadioCoordinator(DataUpdateCoordinator):
    def __init__(self, hass, entry):
        super().__init__(hass, _LOGGER, name='G2H internal Thread radio', config_entry=entry,
                         update_interval=timedelta(seconds=30))
        self.entry = entry
        self.closed = False
        self._pending_read = None
        self._pending_started = None
        self.poll_timing = {'reads':0,'local_timeouts':0,'blocked_reads':0}
    async def close(self):
        self.closed = True
        await self.async_shutdown()
        pending = self._pending_read
        self._pending_read = None
        if pending is not None:
            if not pending.done():
                pending.cancel()
            with suppress(asyncio.CancelledError, Exception):
                await pending
    async def _read(self, attributes, timeout):
        # A local timeout does not cancel the Matter server operation. Retain
        # it so optional table reads cannot queue more operations behind it.
        pending = self._pending_read
        if pending is not None:
            if not pending.done():
                self.poll_timing['blocked_reads'] += 1
                raise TimeoutError
            # A reply from a previous update is not a current radio sample.
            with suppress(asyncio.CancelledError, Exception):
                await pending
            self._pending_read = None
        self._pending_started = self.hass.loop.time()
        pending = asyncio.create_task(
            read_cluster(self.hass, self.entry, CLUSTER, attributes, timeout=30),
            name='G2H single Thread diagnostics read')
        self._pending_read = pending
        self.poll_timing['reads'] += 1
        try:
            return await asyncio.wait_for(asyncio.shield(pending), timeout)
        except TimeoutError:
            self.poll_timing['local_timeouts'] += 1
            raise
        finally:
            elapsed = max(0, int((self.hass.loop.time()-self._pending_started)*1000))
            self.poll_timing['last_read_ms'] = elapsed
            self.poll_timing['max_read_ms'] = max(elapsed,self.poll_timing.get('max_read_ms',0))
            if pending.done() and self._pending_read is pending:
                self._pending_read = None
    async def _async_update_data(self):
        if self.closed:
            raise UpdateFailed('Thread coordinator stopped')
        try:
            # Core fields stay useful on older firmware and when a large table
            # cannot cross the server transport. Never wildcard the whole cluster.
            values = await self._read((0,1,2), timeout=5)
            parse(values)
            try:
                # Entire update: <=5 seconds for core +4 seconds optional budget.
                async with asyncio.timeout(4):
                    ids = (9,10,12,13,15,16,18,19,20,21,22)
                    try:
                        extra = await self._read(ids, timeout=1)
                        values.update({PATH+str(i):extra[PATH+str(i)] for i in ids if PATH+str(i) in extra})
                    except (ValueError, OSError, TimeoutError):
                        pass
                    for fresh_id, table_id in ((9,11),(12,14),(15,17)):
                        if values.get(PATH+str(fresh_id)) is not True:
                            continue
                        try:
                            table = await self._read((table_id,), timeout=1)
                            if PATH+str(table_id) in table:
                                values[PATH+str(table_id)] = table[PATH+str(table_id)]
                        except (ValueError, OSError, TimeoutError):
                            pass
            except TimeoutError:
                pass
            return parse(values)
        except (ValueError, OSError, TimeoutError) as err:
            raise UpdateFailed('Cannot read fresh Thread status through Matter') from err
async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = RadioCoordinator(hass, entry)
    hass.data[DOMAIN][entry.entry_id]['radio'] = coordinator
    # A missing/stale radio must not prevent the existing camera from loading.
    await coordinator.async_refresh()
    async_add_entities([RadioSensor(coordinator, entry, key, name) for key,name in NAMES.items()])
class RadioSensor(CoordinatorEntity, SensorEntity):
    _attr_has_entity_name = True
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    def __init__(self, coordinator, entry, key, name):
        super().__init__(coordinator)
        self.key = key
        self._attr_name = name
        # Existing role/channel IDs are intentionally unchanged.
        self._attr_unique_id = entry.data['unique_id'] + '-thread-' + key
        client = coordinator.hass.data[DOMAIN][entry.entry_id]['client']
        self._attr_device_info = DeviceInfo(identifiers={get_node_device_identifier(client.server_info, entry.data['node_id'])})
        if key == 'uptime_ms':
            self._attr_native_unit_of_measurement = 'ms'
    @property
    def available(self):
        return super().available and bool(self.coordinator.data) and self.coordinator.data.get(self.key) is not None
    @property
    def native_value(self):
        return self.coordinator.data.get(self.key) if self.coordinator.data else None
    @property
    def extra_state_attributes(self):
        if self.key in ('neighbors','children','routers') and self.coordinator.data:
            table = self.coordinator.data.get(self.key + '_table')
            return {'radio_table': table} if table is not None else {}
        return {}
