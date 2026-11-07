"""Optional diagnostics on the existing Matter pairing; no commissioning."""
from homeassistant.const import Platform
DOMAIN = "g2h_thread_diagnostics"
async def async_setup_entry(hass, entry):
    await hass.config_entries.async_forward_entry_setups(entry, [Platform.SENSOR])
    return True
async def async_unload_entry(hass, entry):
    return await hass.config_entries.async_unload_platforms(entry, [Platform.SENSOR])
