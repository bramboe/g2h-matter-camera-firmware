"""Verify live standard descriptors instead of requiring cached camera attributes."""
PATHS=['0/40/3','0/40/15','0/40/18','1/29/1']
def camera_identity(attributes):
    product=attributes.get('0/40/3');serial=attributes.get('0/40/15');unique=attributes.get('0/40/18');servers=attributes.get('1/29/1')
    if product!='Aqara G2H Thread & Matter Camera' or not isinstance(serial,str) or not serial.startswith('g2hcam-'):return None
    if not isinstance(unique,str) or not 1<=len(unique)<=32:return None
    if not isinstance(servers,list) or not {1361,1363}.issubset(servers):return None
    return unique
