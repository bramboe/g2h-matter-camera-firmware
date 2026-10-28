"""Attach camera platforms to verified nodes on HA's existing Matter fabric."""
import asyncio
from contextlib import suppress
from datetime import timedelta
import logging

from homeassistant.components.matter.helpers import get_matter
from .discovery import is_camera_candidate

DOMAIN = 'g2h_matter_camera'
DATA_AUTO_ATTACH = 'g2h_matter_camera_auto_attach'
_LOGGER = logging.getLogger(__name__)


class AutoAttach:
    """Check the local node cache; only new G2H candidates cause live reads.

    No network discovery, commissioning, cache interviews or repeated reads of
    configured cameras. Import flows verify live identity before creating entries.
    """

    def __init__(self, hass):
        self.hass = hass
        self.task = None
        self.closed = False
        self.cancel_interval = None
        self.cancel_started = None
        self.counters = {'scans': 0, 'created': 0, 'already_configured': 0, 'failed': 0}

    async def async_scan(self, _event=None):
        if self.closed or self.task is not None:
            return
        self.task = asyncio.current_task()
        try:
            matter_entries = self.hass.config_entries.async_loaded_entries('matter')
            if len(matter_entries) != 1:
                return  # get_matter() is HA's single-adapter API.
            try:
                client = get_matter(self.hass).matter_client
                nodes = list(client.get_nodes())
            except (IndexError, KeyError, RuntimeError):
                return  # Startup/reload can temporarily leave the adapter absent.
            self.counters['scans'] += 1
            matter_id = matter_entries[0].entry_id
            for node in nodes:
                if self.closed or not is_camera_candidate(node.node_data.attributes):
                    continue
                # Include disabled entries: automatic detection must respect them.
                entries = self.hass.config_entries.async_entries(DOMAIN)
                if any(entry.data.get('matter_entry_id') == matter_id and
                       entry.data.get('node_id') == node.node_id for entry in entries):
                    continue
                result = await self.hass.config_entries.flow.async_init(
                    DOMAIN, context={'source': 'import'},
                    data={'matter_entry_id': matter_id, 'node_id': node.node_id})
                if result.get('type') == 'create_entry':
                    self.counters['created'] += 1
                elif result.get('reason') == 'already_configured':
                    self.counters['already_configured'] += 1
                else:
                    self.counters['failed'] += 1
        except Exception:
            # Retry on the next cache scan; never log node data or server replies.
            self.counters['failed'] += 1
            _LOGGER.debug('Automatic G2H attachment deferred')
        finally:
            self.task = None

    async def async_close(self, _event=None):
        self.closed = True
        for cancel in (self.cancel_interval, self.cancel_started):
            if cancel is not None:
                cancel()
        if self.task is not None and self.task is not asyncio.current_task():
            self.task.cancel()
            with suppress(asyncio.CancelledError):
                await self.task


def start_auto_attach(hass):
    """One observer per HA instance, including before any G2H is paired."""
    from homeassistant.const import EVENT_HOMEASSISTANT_STARTED, EVENT_HOMEASSISTANT_STOP
    from homeassistant.helpers.event import async_track_time_interval

    if DATA_AUTO_ATTACH in hass.data:
        return
    scanner = hass.data[DATA_AUTO_ATTACH] = AutoAttach(hass)
    scanner.cancel_interval = async_track_time_interval(
        hass, scanner.async_scan, timedelta(seconds=30))
    hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STOP, scanner.async_close)
    if hass.is_running:
        hass.async_create_task(scanner.async_scan())
    else:
        scanner.cancel_started = hass.bus.async_listen_once(
            EVENT_HOMEASSISTANT_STARTED, scanner.async_scan)
