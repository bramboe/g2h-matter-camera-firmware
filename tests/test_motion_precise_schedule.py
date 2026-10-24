"""True 500-ms scheduling under HA2026.10, without floor-time bursts."""
import asyncio
import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('g2h_platform_schedule_fixture',Path(__file__).with_name('test_camera_platforms.py'))
fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
motion=fixture.motion;diagnostics=fixture.diagnostics
class Handle:
    def __init__(self,when,callback):self.when=when;self.callback=callback;self.cancelled=False
    def cancel(self):self.cancelled=True
class FractionalLoop:
    def __init__(self):self.now=0.;self.handles=[]
    def time(self):return self.now
    def call_later(self,delay,callback):
        handle=Handle(self.now+delay,callback);self.handles.append(handle);return handle
    def active(self):return [handle for handle in self.handles if not handle.cancelled]
class PreciseSchedule(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):await fixture.Platforms.asyncSetUp(self)
    async def test_fractional_phase_one_timer_pref_retry_and_unload(self):
        loop=FractionalLoop();self.hass.loop=loop;coordinator=motion.MotionCoordinator(self.hass,self.entry)
        for phase in (.001,.05,.49,.501,.95,.999):
            loop.now=24+phase;coordinator._schedule_refresh()
            self.assertEqual(len(loop.active()),1);self.assertAlmostEqual(loop.active()[0].when-loop.now,.5)
        # A parent refresh helper clearing its slot must not orphan our timer.
        coordinator._unsub_refresh=None;coordinator._schedule_refresh();self.assertEqual(len(loop.active()),1)
        coordinator._retry_after=3;coordinator._schedule_refresh();self.assertAlmostEqual(loop.active()[0].when-loop.now,3)
        self.assertIsNone(coordinator._retry_after)
        coordinator._retry_after=.01;coordinator._schedule_refresh();self.assertAlmostEqual(loop.active()[0].when-loop.now,.5)
        self.entry.pref_disable_polling=True;coordinator._schedule_refresh();self.assertFalse(loop.active())
        self.entry.pref_disable_polling=False;coordinator._schedule_refresh();self.assertEqual(len(loop.active()),1)
        await coordinator.close();self.assertFalse(loop.active());self.assertIsNone(coordinator._unsub_refresh)
        coordinator._schedule_refresh();self.assertFalse(loop.active())
    async def test_real_polls_are_spaced_after_completion(self):
        times=[];p=diagnostics.MOTION_PATH
        async def response(node,paths):
            times.append(asyncio.get_running_loop().time());await asyncio.sleep(.02)
            return {p+'0':True,p+'1':False,p+'2':True,p+'3':'native-held-motion',p+'5':0,p+'6':2000}
        self.client.read_attribute.side_effect=response
        coordinator=motion.MotionCoordinator(self.hass,self.entry)
        await coordinator.async_refresh();coordinator._schedule_refresh();await asyncio.sleep(1.13);await coordinator.close()
        self.assertGreaterEqual(len(times),2);self.assertLessEqual(len(times),3)
        for before,after in zip(times,times[1:]):self.assertGreaterEqual(after-before,.49)
        count=len(times);await asyncio.sleep(.08);self.assertEqual(len(times),count)
    async def test_inflight_read_does_not_queue_another_poll(self):
        times=[];p=diagnostics.MOTION_PATH
        async def response(node,paths):
            times.append(asyncio.get_running_loop().time());await asyncio.sleep(.7)
            return {p+'0':True,p+'1':False,p+'2':True,p+'3':'native-held-motion',p+'5':0,p+'6':2000}
        self.client.read_attribute.side_effect=response
        coordinator=motion.MotionCoordinator(self.hass,self.entry);coordinator._schedule_refresh()
        await asyncio.sleep(.55);coordinator._schedule_refresh() # As an expiry/recovery update can do.
        await asyncio.sleep(1.25);await coordinator.close()
        self.assertGreaterEqual(len(times),1);self.assertLessEqual(len(times),2)
        for before,after in zip(times,times[1:]):self.assertGreaterEqual(after-before,1.19)
        self.assertIsNone(coordinator._precise_task);self.assertIsNone(coordinator._unsub_refresh)
if __name__=='__main__':unittest.main()
