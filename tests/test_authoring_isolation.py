"""Isolation checks: a capability probe must never adopt another editor project."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import probe_psd2live as probe


class AuthoringIsolationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.run = Path(self.temp.name)
        self.manifest = {'schema_version': 1, 'state': 'history-owned',
                         'operations': [], 'exports': [{'state': 'history-source'}]}
        probe.write_json(self.run / 'probe.json', self.manifest)

    def session(self):
        with patch.object(probe, 'initialize', return_value=(
                {'result': {'serverInfo': {'version': '2.0.2'}}}, 'session')):
            return probe.ProbeSession(self.run)

    def test_source_restore_refuses_foreign_editor_state(self):
        session = self.session()
        with patch.object(probe, 'call', return_value=(
                {'result': {'structuredContent': {'state': 'history-someone-else'}}}, 'session')) as call:
            with self.assertRaisesRegex(RuntimeError, 'outside this experiment'):
                session.restore_source()
        self.assertEqual([args.args[1]['name'] for args in call.call_args_list], ['inspect'])
        self.assertEqual(session.state, 'history-owned')
        self.assertEqual(json.loads((self.run / 'probe.json').read_text(encoding='utf8'))['state'], 'history-owned')

    def test_read_only_skeleton_cannot_adopt_foreign_state(self):
        session = self.session()
        with patch.object(probe, 'call', return_value=(
                {'result': {'structuredContent': {'state': 'history-other'}}}, 'session')):
            with self.assertRaisesRegex(RuntimeError, 'outside this experiment'):
                session.invoke('skeleton', {'request': {'mode': 'get'}})
        self.assertEqual(session.state, 'history-owned')

    def test_noop_is_evidence_not_success(self):
        session = self.session()
        with patch.object(probe, 'call', return_value=(
                {'result': {'structuredContent': {'state': 'history-owned', 'applied': False}}}, 'session')):
            with self.assertRaisesRegex(RuntimeError, 'applied:false'):
                session.invoke('form', {'state': session.state, 'changes': []})
        record = json.loads((self.run / 'operations/000-form.json').read_text(encoding='utf8'))
        self.assertFalse(record['response']['applied'])
        self.assertEqual(session.state, 'history-owned')

    def test_create_refuses_loaded_project_before_asset_write(self):
        session = self.session()
        with patch.object(session, 'invoke', return_value={'loaded': True}) as invoke:
            with self.assertRaisesRegex(RuntimeError, 'loaded project'):
                probe.create_probe(session)
        invoke.assert_called_once_with('inspect', {'scope': 'project'})
        self.assertFalse((self.run / 'input').exists())


if __name__ == '__main__':
    unittest.main()
