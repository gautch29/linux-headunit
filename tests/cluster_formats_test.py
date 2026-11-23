# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from cluster_layout import format_snapshot as snapshot, format_choice as choice


class ClusterFormatsTests(unittest.TestCase):
    def test_native_formats_have_explicit_saved_selection_and_map_gating(self):
        model = snapshot('sport', False, None)
        self.assertEqual(model['native']['selected'], 'sport')
        self.assertEqual(model['native']['selectionKind'], 'saved_preference')
        for identifier in ('minimal', 'sport', 'gauges'):
            self.assertEqual(choice(model, 'native', identifier)['page'], identifier)
        with self.assertRaises(ValueError):
            choice(model, 'native', 'map')
        self.assertEqual(choice(snapshot('map', True, None), 'native', 'map')['page'], 'map')
        self.assertEqual(snapshot(None, False, None)['native']['choices'], [])

    def test_signed_catalogue_must_declare_the_exact_layout_and_confirmation(self):
        style = '8BC9ABB1-CB20-595E-92BF-D8F099BF3515'
        state = {'choices': [{'id': style, 'name': 'DigiRing'}], 'active': style,
                 'confirmedActive': '', 'pending': style, 'error': '',
                 'ready': True, 'transitioning': False}
        model = snapshot('media', False, state)
        self.assertEqual(choice(model, 'ultra', 'sport')['layout'], style)
        self.assertEqual(model['ultra']['active'], 'sport')
        self.assertIsNone(model['ultra']['confirmed'])
        self.assertEqual(model['ultra']['pending'], 'sport')
        with self.assertRaises(ValueError):
            choice(model, 'ultra', 'minimal')
        state['confirmedActive'] = style
        self.assertEqual(snapshot('media', False, state)['ultra']['confirmed'], 'sport')
        for field in ('ready', 'idle'):
            blocked = dict(state, **{field: False})
            with self.assertRaises(ValueError):
                choice(snapshot('media', False, blocked), 'ultra', 'sport')
        state['choices'][0]['name'] = 'Foreign'
        foreign = snapshot('media', False, state)
        self.assertEqual(choice(foreign, 'ultra', style)['name'], 'Foreign')
        with self.assertRaises(ValueError):
            choice(foreign, 'ultra', 'sport')
        for renderer, identifier in (('ultra', 'sport'), ('unknown', 'sport'), ('native', None)):
            with self.assertRaises(ValueError):
                choice(snapshot('media', False, None), renderer, identifier)


if __name__ == '__main__':
    unittest.main()
