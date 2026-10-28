"""Validate nullable revision-2 fields; absent/stale does not become zero."""
CLUSTER = 0xFFF1FC10
PATH = f"0/{CLUSTER}/"
FIELDS = {"neighbors": (9, 10), "children": (12, 13), "routers": (15, 16),
          "tx_total": (18, 19), "rx_total": (18, 20), "tx_retry": (18, 21),
          "uptime_ms": (18, 22)}
def unsigned(value, maximum):
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= maximum

def parse(values):
    if not isinstance(values, dict) or values.get(PATH + '0') is not True:
        raise ValueError('Thread radio status unavailable')
    role, channel = values.get(PATH + '1'), values.get(PATH + '2')
    if role not in ('disabled', 'detached', 'child', 'router', 'leader') or not unsigned(channel, 26) or channel < 11:
        raise ValueError('Invalid Thread radio status')
    out = {'role': role, 'channel': channel}
    for name, (fresh_id, value_id) in FIELDS.items():
        value = values.get(PATH + str(value_id))
        maximum = 64 if name in ('neighbors','children','routers') else (2**64-1 if name == 'uptime_ms' else 2**32-1)
        out[name] = value if values.get(PATH + str(fresh_id)) is True and unsigned(value, maximum) else None
    for name, fresh, table in (('neighbors',9,11),('children',12,14),('routers',15,17)):
        value = values.get(PATH + str(table))
        out[name + '_table'] = value if values.get(PATH + str(fresh)) is True and isinstance(value,str) and len(value) <= 12288 else None
    return out
