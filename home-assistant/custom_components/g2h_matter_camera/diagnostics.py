"""Read-only helpers for diagnostics on the already verified Matter node."""
import asyncio
import re
from aiohttp import ClientError
from homeassistant.components.matter.helpers import get_matter
from matter_server.common.errors import MatterError
from matter_server.client.exceptions import MatterClientException
from . import DOMAIN
MOTION_CLUSTER = 0xFFF1FC12
MOTION_PATH = f'0/{MOTION_CLUSTER}/'

def motion_values(values):
    """A clear motion sample is false; unknown/stale/disabled is None."""
    if not isinstance(values, dict):
        raise ValueError('Invalid motion response')
    fresh = values.get(MOTION_PATH + '0') is True
    motion, enabled = values.get(MOTION_PATH + '1'), values.get(MOTION_PATH + '2')
    enabled = enabled if isinstance(enabled, bool) else None
    reason = values.get(MOTION_PATH + '3')
    if not isinstance(reason, str) or not re.fullmatch('[a-z][a-z-]{0,79}', reason):
        reason = 'unavailable'
    age, window = values.get(MOTION_PATH + '5'), values.get(MOTION_PATH + '6')
    age = age if isinstance(age, int) and not isinstance(age, bool) and 0 <= age <= 2**32-1 else None
    window = window if isinstance(window, int) and not isinstance(window, bool) and 1 <= window <= 65535 else None
    # The producer is sampled independently; reject internally inconsistent data.
    if age is not None and window is not None and age > window:
        fresh = False
    known = (fresh and enabled is True and isinstance(motion, bool) and
             age is not None and window is not None and age <= window)
    return {'motion': motion if known else None, 'producer_enabled': enabled,
            'reason': reason, 'sample_age_ms': age, 'freshness_window_ms': window}

async def read_cluster(hass, entry, cluster, attributes=None, timeout=10):
    """Use HA's existing client and identity; never create or close its socket."""
    matter_entry = hass.config_entries.async_get_entry(entry.data['matter_entry_id'])
    if matter_entry is None:
        raise ValueError('Existing Matter integration unavailable')
    try:
        runtime = hass.data[DOMAIN][entry.entry_id]
        client = runtime['client']
        if get_matter(hass).matter_client is not client:
            raise ValueError('Existing Matter client changed')
        path = f'0/{cluster}/'
        paths = path + '*' if attributes is None else [path + str(i) for i in attributes]
        async with asyncio.timeout(timeout):
            values = await client.read_attribute(entry.data['node_id'], paths)
    except (IndexError, KeyError, RuntimeError, MatterError, MatterClientException, ClientError) as err:
        raise ValueError('Cannot read existing Matter diagnostics') from err
    if not isinstance(values, dict):
        raise ValueError('Invalid Matter diagnostics response')
    return values


def _exception_classes(error):
    """Error types only: messages and payloads can contain private values."""
    def name(value):
        text = type(value).__name__ if value is not None else None
        return text if isinstance(text, str) and re.fullmatch('[A-Za-z_][A-Za-z0-9_]{0,63}', text) else None
    return {'error_class': name(error), 'cause_class': name(getattr(error, '__cause__', None))}


def _core_probe(values):
    path = f'0/{0xFFF1FC10}/'
    if not isinstance(values, dict):
        return {'result_key_count': 0, 'fresh': None, 'role': None, 'channel': None}
    role, channel, fresh = values.get(path+'1'), values.get(path+'2'), values.get(path+'0')
    return {'result_key_count': min(len(values),65535),
            'fresh': fresh if isinstance(fresh,bool) else None,
            'role': role if role in ('disabled','detached','child','router','leader') else None,
            'channel': channel if isinstance(channel,int) and not isinstance(channel,bool) and 11<=channel<=26 else None}


def _coordinator_summary(coordinator, motion=False):
    if coordinator is None:
        return None
    data = coordinator.data if isinstance(coordinator.data,dict) else {}
    result = {'last_update_success': bool(coordinator.last_update_success),
              'closed': bool(getattr(coordinator,'closed',False)),
              **_exception_classes(getattr(coordinator,'last_exception',None))}
    if motion:
        timing=getattr(coordinator,'poll_timing',{})
        if isinstance(timing,dict):
            result['poll_timing']={key:timing[key] for key in ('reads','expiries','expired_during_read','native_stale','errors','retained_on_error','coalesced_waits','expired_responses','last_read_ms','max_read_ms','remaining_at_response_ms','expiry_sample_age_ms') if type(timing.get(key)) is int and 0<=timing[key]<=2**63-1}
        fresh = bool(getattr(coordinator,'fresh',False))
        reason = data.get('reason')
        result['fresh'] = fresh
        result['motion'] = data.get('motion') if fresh and isinstance(data.get('motion'),bool) else None
        result['producer_enabled'] = data.get('producer_enabled') if isinstance(data.get('producer_enabled'),bool) else None
        result['reason'] = reason if isinstance(reason,str) and re.fullmatch('[a-z][a-z-]{0,79}',reason) else None
        for key in ('sample_age_ms','freshness_window_ms'):
            value=getattr(coordinator,'_sample_age_ms',data.get(key)) if key=='sample_age_ms' else data.get(key)
            result[key]=value if isinstance(value,int) and not isinstance(value,bool) and 0<=value<=2**32-1 else None
    else:
        timing=getattr(coordinator,'poll_timing',{})
        if isinstance(timing,dict):
            result['poll_timing']={key:timing[key] for key in ('reads','local_timeouts','blocked_reads','last_read_ms','max_read_ms') if type(timing.get(key)) is int and 0<=timing[key]<=2**63-1}
        role=data.get('role');result['role']=role if role in ('disabled','detached','child','router','leader') else None
        for key in ('channel','neighbors','children','routers','tx_total','rx_total','tx_retry','uptime_ms'):
            value=data.get(key)
            result[key]=value if isinstance(value,int) and not isinstance(value,bool) and 0<=value<=2**64-1 else None
        result['table_lengths']={key:len(data[key+'_table']) if isinstance(data.get(key+'_table'),str) and len(data[key+'_table'])<=12288 else None
                                 for key in ('neighbors','children','routers')}
    return result


async def async_get_config_entry_diagnostics(hass, entry):
    """Export public runtime plus two bounded read-only probes of the same node.

    Never returns entry.data, URLs, raw exceptions/results, table contents,
    fabric/dataset identifiers, addresses, SDP, credentials, or private media.
    """
    from contextlib import suppress
    from homeassistant.const import CONF_URL
    from homeassistant.helpers.aiohttp_client import async_get_clientsession
    from .protocol import MatterWire

    # connect() itself closes on failure/cancellation; bound that cleanup too.
    class DiagnosticWire(MatterWire):
        async def close(self):
            try:
                async with asyncio.timeout(.5):
                    await super().close()
            except TimeoutError:
                pass
            finally:
                if self.reader is not None:
                    self.reader.cancel()

    runtime = hass.data.get(DOMAIN,{}).get(entry.entry_id,{})
    node_available = getattr(runtime.get('node'),'available',None)
    output={'extension_version':'0.4.1',
            'cached_node_available':node_available if isinstance(node_available,bool) else None,
            'radio':_coordinator_summary(runtime.get('radio')),
            'motion':_coordinator_summary(runtime.get('motion'),True)}
    scanner=hass.data.get('g2h_matter_camera_auto_attach')
    if scanner is not None:
        output['auto_attach']={'enabled':not scanner.closed,
            **{key:value for key,value in scanner.counters.items()
               if key in ('scans','created','already_configured','failed') and type(value) is int and value>=0}}
    snapshot=runtime.get('snapshot')
    if isinstance(snapshot,dict):
        output['snapshot']={key:value for key,value in snapshot.items()
            if key in ('stage','error_class','data_type','resolution_type','imageCodec_type','encoded_length','width','height','jpeg_bytes')
            and (type(value) is int or isinstance(value,str) and re.fullmatch('[A-Za-z_-]{1,50}',value))}
    refresh=runtime.get('cache_refresh')
    if isinstance(refresh,dict):
        output['cache_refresh']={key:refresh.get(key) if isinstance(refresh.get(key),bool) else None
                                 for key in ('attempted','succeeded','cached_node_available_after')}
        for key in ('error_class','cause_class'):
            value=refresh.get(key)
            output['cache_refresh'][key]=value if isinstance(value,str) and re.fullmatch('[A-Za-z_][A-Za-z0-9_]{0,63}',value) else None
    try:
        values=await read_cluster(hass,entry,0xFFF1FC10,(0,1,2),timeout=3)
        output['official_client_core']={**_core_probe(values),**_exception_classes(None)}
    except Exception as err:
        output['official_client_core']={**_core_probe(None),**_exception_classes(err)}

    wire=None
    try:
        async with asyncio.timeout(3):
            matter_entry=hass.config_entries.async_get_entry(entry.data['matter_entry_id'])
            if matter_entry is None:
                raise ValueError('Existing Matter integration unavailable')
            wire=DiagnosticWire(async_get_clientsession(hass),matter_entry.data[CONF_URL])
            await wire.connect()
            values=await wire.request('read_attribute',node_id=entry.data['node_id'],
                                      attribute_path=[f'0/{0xFFF1FC10}/{i}' for i in (0,1,2)])
            output['independent_wire_core']={**_core_probe(values),**_exception_classes(None)}
    except Exception as err:
        output['independent_wire_core']={**_core_probe(None),**_exception_classes(err)}
    finally:
        if wire is not None:
            with suppress(Exception):
                await wire.close()
    return output
