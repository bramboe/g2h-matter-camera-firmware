"""Choose a radio already paired through Home Assistant's Matter integration."""
from aiohttp import ClientError
import voluptuous as vol
from homeassistant import config_entries
from homeassistant.components.matter.helpers import get_matter
from . import DOMAIN
from .telemetry import PATH, parse
from .wire import read
class ConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1
    async def async_step_user(self, user_input=None):
        entries = self.hass.config_entries.async_loaded_entries('matter')
        if not entries:
            return self.async_abort(reason='matter_not_ready')
        client = get_matter(self.hass).matter_client
        choices = {}
        for node in client.get_nodes():
            attrs = node.node_data.attributes
            label = str(attrs.get('0/40/5') or attrs.get('0/40/3') or '')
            if PATH + '0' in attrs or 'g2h' in label.lower():
                choices[str(node.node_id)] = f'{label or "G2H"} ({node.node_id})'
        if not choices:
            return self.async_abort(reason='no_radio')
        errors = {}
        if user_input:
            key = str(user_input['node_id'])
            if key not in choices:
                errors['base'] = 'read_failed'
            else:
                try:
                    parse(await read(self.hass, entries[0].entry_id, int(key)))
                except (ValueError, OSError, TimeoutError, ClientError):
                    errors['base'] = 'read_failed'
                else:
                    unique = f'{entries[0].entry_id}-{key}'
                    await self.async_set_unique_id(unique)
                    self._abort_if_unique_id_configured()
                    return self.async_create_entry(title='G2H Thread diagnostics', data={
                        'matter_entry_id': entries[0].entry_id, 'node_id': int(key)})
        return self.async_show_form(step_id='user', data_schema=vol.Schema({
            vol.Required('node_id'): vol.In(choices)}), errors=errors)
