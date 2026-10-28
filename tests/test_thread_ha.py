import importlib.util
from pathlib import Path
import unittest
p=Path(__file__).parents[1]/'home-assistant/custom_components/g2h_thread_diagnostics/telemetry.py'
s=importlib.util.spec_from_file_location('telemetry',p);t=importlib.util.module_from_spec(s);s.loader.exec_module(t)
class TestThreadTelemetry(unittest.TestCase):
    def base(self):return {t.PATH+'0':True,t.PATH+'1':'router',t.PATH+'2':25}
    def test_older_firmware(self):
        d=t.parse(self.base());self.assertIsNone(d['neighbors']);self.assertEqual(d['role'],'router')
    def test_stale_and_null(self):
        d=self.base();d.update({t.PATH+'9':False,t.PATH+'10':4,t.PATH+'12':True,t.PATH+'13':None})
        self.assertIsNone(t.parse(d)['neighbors']);self.assertIsNone(t.parse(d)['children'])
    def test_valid_counts_not_usage(self):
        d=self.base();d.update({t.PATH+'9':True,t.PATH+'10':0,t.PATH+'18':True,t.PATH+'19':4294967295})
        out=t.parse(d);self.assertEqual(out['neighbors'],0);self.assertEqual(out['tx_total'],4294967295);self.assertNotIn('forwarded',out)
    def test_malformed(self):
        for value in (True,-1,65,'4',{},2**64):
            d=self.base();d.update({t.PATH+'9':True,t.PATH+'10':value});self.assertIsNone(t.parse(d)['neighbors'])
        for value in (False,None,'true'):
            d=self.base();d[t.PATH+'0']=value
            with self.assertRaises(ValueError):t.parse(d)
if __name__=='__main__':unittest.main()
