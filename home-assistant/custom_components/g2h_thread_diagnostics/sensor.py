"""Raw radio diagnostics attached to the existing Matter device."""
from aiohttp import ClientError
import logging
from datetime import timedelta
from homeassistant.components.sensor import SensorEntity
from homeassistant.components.matter.helpers import get_matter, get_node_device_identifier
from homeassistant.const import EntityCategory
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity, DataUpdateCoordinator, UpdateFailed
from .telemetry import parse
from .wire import read
_LOGGER = logging.getLogger(__name__)
NAMES = {'role':'Thread role', 'channel':'Thread channel', 'neighbors':'Thread neighbors',
         'children':'Attached Thread children', 'routers':'Known Thread routers',
         'tx_total':'Radio MAC transmitted packets', 'rx_total':'Radio MAC received packets',
         'tx_retry':'Radio MAC retries', 'uptime_ms':'Thread radio uptime'}
class RadioCoordinator(DataUpdateCoordinator):
    def __init__(self, hass, entry):
        super().__init__(hass, _LOGGER, name='G2H Thread diagnostics', config_entry=entry,
                         update_interval=timedelta(seconds=30))
        self.entry = entry
    async def _async_update_data(self):
        try:
            return parse(await read(self.hass, self.entry.data['matter_entry_id'], self.entry.data['node_id']))
        except (ValueError, OSError, TimeoutError, ClientError) as err:
            raise UpdateFailed('Cannot read fresh Thread diagnostics through Matter') from err
async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = RadioCoordinator(hass, entry)
    await coordinator.async_config_entry_first_refresh()
    info = get_matter(hass).matter_client.server_info
    device = DeviceInfo(identifiers={get_node_device_identifier(info, entry.data['node_id'])})
    async_add_entities([RadioSensor(coordinator, entry, device, key, name) for key,name in NAMES.items()])
class RadioSensor(CoordinatorEntity, SensorEntity):
    _attr_has_entity_name = True
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    def __init__(self, coordinator, entry, device, key, name):
        super().__init__(coordinator)
        self.key = key
        self._attr_name = name
        self._attr_unique_id = f'{entry.unique_id}-{key}'
        self._attr_device_info = device
        if key == 'uptime_ms': self._attr_native_unit_of_measurement = 'ms'
    @property
    def available(self):
        return super().available and self.coordinator.data.get(self.key) is not None
    @property
    def native_value(self):
        return self.coordinator.data.get(self.key)

    @property
    def extra_state_attributes(self):
        if self.key in ('neighbors','children','routers'):
            table = self.coordinator.data.get(self.key + '_table')
            return {'radio_table': table} if table is not None else {}
        return {}
