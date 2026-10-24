"""Use HA's existing Matter client/fabric; no new socket or commissioning."""
import asyncio
from homeassistant.components.matter.helpers import get_matter
from matter_server.common.errors import MatterError
from .telemetry import PATH
async def read(hass, matter_entry_id, node_id):
    entries = hass.config_entries.async_loaded_entries('matter')
    # The current HA Matter helper intentionally supports one connection only.
    if not entries or entries[0].entry_id != matter_entry_id:
        raise ValueError('Existing Matter integration unavailable')
    client = get_matter(hass).matter_client
    try:
        async with asyncio.timeout(20):
            # Wildcard gracefully omits revision-2 attributes on older G2H code.
            # The official client enforces READ_ATTRIBUTE's schema requirement.
            result = await client.read_attribute(node_id, PATH + '*')
    except MatterError as err:
        raise ValueError('Matter diagnostics read refused') from err
    if not isinstance(result, dict):
        raise ValueError('Invalid Matter diagnostics response')
    return result
