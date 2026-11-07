"""Select an existing Matter node; never commission another identity."""
from aiohttp import ClientError
import voluptuous as vol
import json
from pathlib import Path
from homeassistant import config_entries
from homeassistant.const import CONF_URL
from homeassistant.components.matter.helpers import get_matter
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from . import DOMAIN
from .protocol import MatterWire,MatterCameraError,MatterCameraCompatibilityError
from .discovery import PATHS,camera_identity,is_camera_candidate
class ConfigFlow(config_entries.ConfigFlow,domain=DOMAIN):
    VERSION=1
    async def async_step_import(self,info):
        """Verify an already commissioned G2H; never commission a new fabric."""
        entries=self.hass.config_entries.async_loaded_entries('matter')
        if len(entries)!=1 or info.get('matter_entry_id')!=entries[0].entry_id:
            return self.async_abort(reason='matter_not_ready')
        node_id=info.get('node_id')
        if type(node_id) is not int or node_id<0:
            return self.async_abort(reason='no_camera')
        for entry in self.hass.config_entries.async_entries(DOMAIN):
            if entry.data.get('matter_entry_id')==entries[0].entry_id and entry.data.get('node_id')==node_id:
                return self.async_abort(reason='already_configured')
        client=get_matter(self.hass).matter_client
        node=next((node for node in client.get_nodes() if node.node_id==node_id),None)
        if node is None or not is_camera_candidate(node.node_data.attributes):
            return self.async_abort(reason='no_camera')
        wire=MatterWire(async_get_clientsession(self.hass),entries[0].data[CONF_URL])
        try:
            await wire.connect()
            wire.require_camera_api()
            live=await wire.request('read_attribute',node_id=node_id,attribute_path=PATHS)
            unique=camera_identity(live) if isinstance(live,dict) else None
            if unique is None:
                return self.async_abort(reason='not_g2h_camera')
        except MatterCameraCompatibilityError:
            return self.async_abort(reason='camera_api_missing')
        except (MatterCameraError,OSError,TimeoutError,ClientError):
            return self.async_abort(reason='matter_read_failed')
        finally:
            await wire.close()
        await self.async_set_unique_id(unique)
        self._abort_if_unique_id_configured()
        return self.async_create_entry(title='G2H Matter-camera',data={
            'node_id':node_id,'unique_id':unique,'matter_entry_id':entries[0].entry_id})

    async def async_step_user(self,user_input=None):
        entries=self.hass.config_entries.async_loaded_entries('matter')
        if not entries:return self.async_abort(reason='matter_not_ready')
        client=get_matter(self.hass).matter_client
        choices={};nodes={}
        for node in client.get_nodes():
            attrs=node.node_data.attributes
            key=str(node.node_id)
            label=attrs.get('0/40/5') or attrs.get('0/40/3') or 'Matter device'
            choices[key]=str(label)+' ('+key+')';nodes[key]=node
        if not choices:return self.async_abort(reason='no_camera')
        info=client.server_info
        schema=getattr(info,'schema_version',None)
        # Export only bounded public troubleshooting metadata. No fabric IDs,
        # credentials, datasets, SDP, serials or unique IDs are written here.
        diagnostic={'extension_version':'0.5.2','server_schema':schema,
          'server_sdk_version':str(getattr(info,'sdk_version','unknown'))[:80],
          'candidate_nodes':[{'node_id':n.node_id,'available':n.available,
            'product_name':n.node_data.attributes.get('0/40/3'),
            'node_label':n.node_data.attributes.get('0/40/5')}
            for n in nodes.values() if n.node_id in (64,68) or 'g2h' in str(n.node_data.attributes.get('0/40/3','')).lower() or 'g2h' in str(n.node_data.attributes.get('0/40/5','')).lower()]}
        diagnostic_path=Path(self.hass.config.path('g2h_matter_camera_diagnostic.json'))
        await self.hass.async_add_executor_job(diagnostic_path.write_text,json.dumps(diagnostic,indent=2))
        errors={};placeholders={'schema':str(schema) if isinstance(schema,int) else 'unknown'}
        if user_input is not None:
            key=str(user_input['node_id'])
            if key not in nodes:errors['base']='no_camera'
            elif nodes[key].node_data.attributes.get('0/40/3') not in (None,'','Aqara G2H Thread & Matter Camera'):
                errors['base']='not_g2h_camera'
            else:
                wire=MatterWire(async_get_clientsession(self.hass),entries[0].data[CONF_URL])
                try:
                    await wire.connect()
                    live=await wire.request('read_attribute',node_id=int(key),attribute_path=PATHS)
                    unique=camera_identity(live) if isinstance(live,dict) else None
                    if unique is None:errors['base']='not_g2h_camera'
                    else:wire.require_camera_api()
                except MatterCameraCompatibilityError as err:
                    errors['base']='camera_api_missing';placeholders['schema']=str(err.schema)
                except (MatterCameraError,OSError,TimeoutError,ClientError):errors['base']='matter_read_failed'
                finally:await wire.close()
                if not errors:
                    await self.async_set_unique_id(unique);self._abort_if_unique_id_configured()
                    return self.async_create_entry(title='G2H Matter-camera',data={'node_id':int(key),'unique_id':unique,'matter_entry_id':entries[0].entry_id})
        return self.async_show_form(step_id='user',data_schema=vol.Schema({vol.Required('node_id',default=str(user_input['node_id']) if user_input else next(iter(choices))):vol.In(choices)}),errors=errors,description_placeholders=placeholders)
