"""Add camera functionality to an already paired G2H Matter node."""
import asyncio
from aiohttp import ClientError
from homeassistant.const import Platform
from homeassistant.components.matter.helpers import get_matter
from homeassistant.exceptions import ConfigEntryNotReady
from matter_server.common.errors import NodeNotExists
from homeassistant.const import CONF_URL
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers import config_validation as cv
from .protocol import MatterWire,MatterCameraError
from .discovery import PATHS,camera_identity
DOMAIN='g2h_matter_camera'
CONFIG_SCHEMA=cv.empty_config_schema(DOMAIN)

async def async_setup(hass,config):
    """Enable automatic attachment without asking for a second pairing."""
    from .auto_attach import start_auto_attach
    start_auto_attach(hass)
    return True

async def async_setup_entry(hass,entry):
    try:
        adapter=get_matter(hass);client=adapter.matter_client
        node=client.get_node(entry.data['node_id'])
    except (IndexError,KeyError,RuntimeError,NodeNotExists) as err:
        raise ConfigEntryNotReady('Existing Matter node is not loaded') from err
    # A controller may cache an older interview without camera clusters or ID.
    # Verify the current node over CASE before binding entities to its node ID.
    matter_entry=hass.config_entries.async_get_entry(entry.data['matter_entry_id'])
    if matter_entry is None:raise ConfigEntryNotReady('Existing Matter integration unavailable')
    wire=MatterWire(async_get_clientsession(hass),matter_entry.data[CONF_URL])
    try:
        await wire.connect()
        attributes=await wire.request('read_attribute',node_id=entry.data['node_id'],attribute_path=PATHS)
        if not isinstance(attributes,dict) or camera_identity(attributes)!=entry.data['unique_id']:
            raise ConfigEntryNotReady('Matter camera identity differs')
    except (MatterCameraError,OSError,TimeoutError,ClientError) as err:
        raise ConfigEntryNotReady('Cannot verify existing Matter camera') from err
    finally:await wire.close()
    # The independent CASE read above proves the configured camera identity.
    # Refresh an offline cached interview once; never alter subscriptions globally
    # or make camera/Thread setup depend on successful cache repair.
    from .diagnostics import _exception_classes
    refresh={'attempted':False,'succeeded':None,**_exception_classes(None)}
    if getattr(node,'available',None) is False:
        refresh['attempted']=True
        try:
            async with asyncio.timeout(15):
                await client.interview_node(entry.data['node_id'])
                node=client.get_node(entry.data['node_id'])
            refresh['succeeded']=True
        except Exception as err:
            refresh.update({'succeeded':False,**_exception_classes(err)})
    available=getattr(node,'available',None)
    refresh['cached_node_available_after']=available if isinstance(available,bool) else None
    hass.data.setdefault(DOMAIN,{})[entry.entry_id]={'node':node,'client':client,'cache_refresh':refresh}
    await hass.config_entries.async_forward_entry_setups(entry,[Platform.CAMERA,Platform.SENSOR,Platform.BINARY_SENSOR,Platform.SWITCH])
    return True
async def async_unload_entry(hass,entry):
    if await hass.config_entries.async_unload_platforms(entry,[Platform.CAMERA,Platform.SENSOR,Platform.BINARY_SENSOR,Platform.SWITCH]):
        runtime=hass.data[DOMAIN].pop(entry.entry_id,None)
        if runtime:
            for name in ('radio','motion','controls'):
                if runtime.get(name):await runtime[name].close()
        return True
    return False
