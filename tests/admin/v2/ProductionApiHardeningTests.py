"""M18.3: production Api/HttpServer + RuntimeWorker + an independent TCP device.

Only model inference is faked. No NPU, installed service or production store.
"""
import unittest
import ManualApiTests as fixture


class ProductionApiHardeningTests(fixture.ManualApiFixture):
    def device(self):
        status, reply = self.api('/api/devices/state')
        self.assertEqual(status, 200)
        return next(device for device in reply['devices'] if device['targetId'] == 'device_fan')

    def property(self):
        return self.device()['properties']['power']

    def poll(self):
        return self.api('/api/devices/poll', {'target_id': 'device_fan', 'expected_hash': self.active['hash']})

    def test_removed_route_and_no_public_forwarder(self):
        before = self.api('/api/devices/state')[1]
        injection = {'target_id': 'device_fan', 'properties': {'power': True}}
        # Unknown route (404), not an existing endpoint with a method restriction (405).
        for method in ('GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS'):
            status, reply = self.api('/api/devices/observe', None if method == 'GET' else injection, method)
            self.assertEqual(status, 404, (method, reply))
            self.assertEqual(reply['errors'][0]['code'], 'not_found')
        for path in ('/api/observe_state', '/api/runtime', '/api/devices/observe_state'):
            status, reply = self.api(path, {'method': 'observe_state', **injection})
            self.assertEqual(status, 404, reply)
        # The actual route registry and the frontend POST allowlist must not advertise it.
        for name in ('src/admin/Api.cpp', 'src/admin/frontend/src/api/admin.ts'):
            source = (fixture.ROOT / name).read_text()
            self.assertNotIn('/api/devices/observe', source)
            self.assertNotIn('observe_state', source)
        self.assertEqual(self.api('/api/devices/state')[1], before)
        self.assertEqual(self.equipment.requests, [])

    def test_prediction_requires_independent_get_to_confirm(self):
        self.assertEqual(self.property()['knowledge'], 'unknown')
        status, reply = self.command()
        self.assertEqual(status, 200, reply)
        self.assertTrue(reply['execution']['success'])
        predicted = self.property()
        self.assertEqual(predicted['knowledge'], 'predicted')
        self.assertIs(predicted['value'], True)
        self.assertEqual([method for method, _ in self.equipment.requests], ['POST'])
        # Status/dashboard/state refresh must neither poll hardware nor promote prediction.
        for path in ('/api/status', '/api/dashboard', '/api/devices/state'):
            self.assertEqual(self.api(path)[0], 200)
        self.assertEqual(self.property(), predicted)
        self.assertEqual(len(self.equipment.requests), 1)

        # External device state changes WITHOUT an Admin request. Its GET disagrees
        # with the command response, proving that Confirmed uses independent evidence.
        self.equipment.power = False
        self.assertEqual(self.poll()[0], 200)
        self.assertEqual(self.property()['knowledge'], 'confirmed')
        self.assertIs(self.property()['value'], False)
        self.equipment.power = True
        self.assertEqual(self.poll()[0], 200)
        self.assertIs(self.property()['value'], True)
        self.assertEqual([method for method, _ in self.equipment.requests], ['POST', 'GET', 'GET'])

    def test_failed_and_unreachable_poll_do_not_confirm(self):
        self.assertEqual(self.command()[0], 200)
        predicted = self.property()
        self.assertEqual(predicted['knowledge'], 'predicted')
        for mode in ('http_error', 'malformed', 'wrong_type'):
            self.equipment.mode = mode
            status, reply = self.poll()
            self.assertEqual(status, 422, reply)
            self.assertFalse(reply['ok'])
            self.assertEqual(self.property(), predicted)
        self.equipment.shutdown()
        self.thread.join(timeout=2)
        self.equipment.server_close()
        status, reply = self.poll()
        self.assertEqual(status, 422, reply)
        self.assertEqual(reply['errors'][0]['code'], 'connection_failure')
        self.assertEqual(self.property(), predicted)
        self.assertEqual(self.device()['connectivity'], 'unreachable')

    def test_state_read_only_and_poll_payload_not_an_injection_seam(self):
        before = self.api('/api/devices/state')[1]
        for method in ('POST', 'PUT', 'PATCH', 'DELETE'):
            status, reply = self.api('/api/devices/state', {'target_id': 'device_fan', 'properties': {'power': True}}, method)
            self.assertEqual(status, 405, reply)
        valid = {'target_id': 'device_fan', 'expected_hash': self.active['hash']}
        for extra in ({'properties': {'power': True}}, {'method': 'observe_state'}, {'timestamp_ms': 1000}):
            self.assertEqual(self.api('/api/devices/poll', {**valid, **extra})[0], 400)
        self.assertEqual(self.api('/api/devices/poll', {**valid, 'expected_hash': '0' * 64})[0], 422)
        self.assertEqual(self.api('/api/devices/state')[1], before)
        self.assertEqual(self.equipment.requests, [])


if __name__ == '__main__':
    unittest.main()
