"""Keep routine product publication on the Consumer route without weakening checks."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
AUDIT = ROOT / 'scripts/check-agents-guidance.ps1'


class ConsumerPublicationPolicyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='consumer-publication-')
        self.fixture = Path(self.temporary.name).resolve()
        self.temporary_root = Path(tempfile.gettempdir()).resolve()
        self.assertEqual(self.fixture.parent, self.temporary_root)
        self.addCleanup(self.cleanup_fixture)
        # Exercise the real auditor with disposable guidance files, without
        # changing the checkout's rules, managed assets or live Git state.
        references = set(re.findall(r"'([^']+\.(?:json|md|ps1|py|hpp))'", AUDIT.read_text()))
        references.update(('AGENTS.md', 'AGENTS.override.md', 'CLAUDE.md',
                           'tests/AGENTS.md', 'tests/CLAUDE.md',
                           'scripts/check-agents-guidance.ps1'))
        for relative in references:
            source = ROOT / relative
            if source.is_file():
                target = self.fixture / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        self.policy_path = self.fixture / 'docs/agents/consumer-publication-policy.json'
        self.original = json.loads(self.policy_path.read_text())
        subprocess.run(['git', 'init', '-q', '-b', 'policy-test', str(self.fixture)], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run(['git', '-C', str(self.fixture), 'add', '--all'], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def cleanup_fixture(self):
        # Recheck the exact disposable target before recursive temporary cleanup.
        self.assertEqual(self.fixture.resolve().parent, self.temporary_root)
        self.assertTrue(self.fixture.name.startswith('consumer-publication-'))
        self.temporary.cleanup()

    def audit(self):
        return subprocess.run(['pwsh', '-NoProfile', '-File', str(AUDIT), '-Root', str(self.fixture)],
                              cwd=self.fixture, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              encoding='utf-8', errors='replace', timeout=30,
                              creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)

    def reject_policy_change(self, section, key, value):
        changed = json.loads(json.dumps(self.original))
        changed[section][key] = value
        self.policy_path.write_text(json.dumps(changed), encoding='utf-8')
        result = self.audit()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(key, result.stdout)

    def test_current_consumer_publication_passes(self):
        result = self.audit()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('PASS: Consumer publication', result.stdout)

    def test_missing_project_override_is_rejected(self):
        (self.fixture / 'AGENTS.override.md').unlink()
        self.assertNotEqual(self.audit().returncode, 0)

    def test_shared_claude_rules_cannot_revert_to_the_provider_route(self):
        (self.fixture / 'CLAUDE.md').write_text('@AGENTS.md\n')
        result = self.audit()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Claude must share', result.stdout)

    def test_consumer_cannot_be_relabelled_as_provider(self):
        self.reject_policy_change('validation', 'scope', 'Provider')

    def test_routine_push_does_not_require_provider_release_ready(self):
        self.reject_policy_change('validation', 'require_release_ready', True)

    def test_consumer_result_does_not_require_a_provider_exception(self):
        self.reject_policy_change('validation', 'consumer_release_ready_false_requires_exception', True)

    def test_current_push_authorization_does_not_require_another_confirmation(self):
        self.reject_policy_change('authorization', 'repeat_confirmation_for_approved_push', True)

    def test_routine_commit_is_not_split_for_provider_evidence(self):
        self.reject_policy_change('commits', 'separate_provider_evidence_commit_required', True)

    def test_product_checks_cannot_be_disabled(self):
        self.reject_policy_change('validation', 'require_product_checks', False)

    def test_push_authority_is_limited_to_the_current_request(self):
        for key in ('commit_only_request_authorizes_push', 'historical_approval_authorizes_new_push'):
            with self.subTest(key=key):
                self.reject_policy_change('authorization', key, True)

    def test_remote_safeguards_cannot_be_disabled(self):
        for key, value in (('require_fresh_observation', False), ('allow_force_push', True),
                           ('verify_remote_head_after_push', False)):
            with self.subTest(key=key):
                self.reject_policy_change('remote', key, value)

    def test_actual_provider_release_gate_is_retained(self):
        self.reject_policy_change('provider_release', 'require_release_ready', False)

    def test_provider_configuration_is_rejected(self):
        (self.fixture / 'agents.json').write_text('{}\n')
        result = self.audit()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must remain a Consumer', result.stdout)


if __name__ == '__main__':
    unittest.main()
