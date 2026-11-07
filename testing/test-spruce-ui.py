#!/usr/bin/env python3
"""Exercise spruce menu semantics without loading SDL or touching a device."""
import os
from enum import Enum
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
os.environ.setdefault('PLATFORM', 'tg5040')
from app.spruce import __main__ as ui, footer
from app.spruce.buttons import Button

class Input(Enum):
    A='A'; B='B'; X='X'; Y='Y'

class Entry:
    def __init__(self, label, **kwargs):self.value=kwargs['value']

class Selection:
    def __init__(self, entry, pressed):self.entry=entry;self.pressed=pressed
    def get_input(self):return self.pressed
    def get_selection(self):return self.entry

class MenuTests(unittest.TestCase):
    def menu(self, rows, presses, **kwargs):
        observed=[]
        class View:
            selected=0
            def get_selection(self, accepted):
                observed.append(ui.FOOTER_CONTEXT())
                key=presses.pop(0)
                if key=='DOWN':self.selected+=1;key=None
                if key is not None:key=Input[key]
                return Selection(Entry('',value=rows[self.selected][0]),key)
            def view_finished(self):pass
        modules={
            'controller.controller_inputs':types.SimpleNamespace(ControllerInput=Input),
            'views.grid_or_list_entry':types.SimpleNamespace(GridOrListEntry=Entry),
            'views.view_creator':types.SimpleNamespace(ViewCreator=types.SimpleNamespace(create_view=lambda *a,**k:View())),
            'views.view_type':types.SimpleNamespace(ViewType=types.SimpleNamespace(ICON_AND_DESC=0)),
        }
        with patch.dict(sys.modules,modules):
            return ui.menu('test', rows, **kwargs),observed

    def test_readonly_a_ignored_and_hints_follow_selection(self):
        rows=[('FC','FC','FCEUmm','✓'),('nickname','Nickname','','')]
        result,observed=self.menu(rows,['A','DOWN','A'],actions={'FC':None,'nickname':'CHANGE'})
        self.assertEqual(result,('nickname','A'))
        self.assertEqual(observed[0],[(Button.B,'BACK')])
        self.assertEqual(observed[-1],[(Button.B,'BACK'),(Button.A,'CHANGE')])

    def test_main_back_says_exit(self):
        _,observed=self.menu([('config','Configuration','','')],['B'],actions={},back='EXIT')
        self.assertEqual(observed[0][0],(Button.B,'EXIT'))

    def test_active_x_ends_and_unwinds(self):
        with patch.object(ui,'session_active',return_value=True),patch.object(ui,'attempt',return_value=True) as action:
            with self.assertRaises(ui.SessionEnded):self.menu([('FC','FC','','')],['X'])
            action.assert_called_once_with('Netplay','Ending session','end')
        self.assertIsNone(ui.FOOTER_CONTEXT)

    def test_inactive_x_does_nothing(self):
        with patch.object(ui,'session_active',return_value=False),patch.object(ui,'attempt') as action:
            result,_=self.menu([('FC','FC','','')],['X','B'])
            self.assertIsNone(result);action.assert_not_called()

    def test_core_list_combines_ds_and_identifies_enabled_dual_cores(self):
        output='\n'.join(['core\tbase\tFC\tFC\tready\t0','core\tbase\tGB\tGB/GBC\tdual\t1',
            'core\tbase\tGBA\tGBA\tpaired\t1','core\textras\tNDS\tNDS (DraStic)\tready\t0',
            'core\textras\tDSP\tNDS (DSperate)\tready\t0','core_hint\tFC\tFCEUmm','gambatte_dual=1','mgba_dual=1'])
        rows,actions=ui.core_rows(output);r={r[0]:r for r in rows}
        self.assertEqual(len(rows),4)
        self.assertEqual(r['NDS'][2],'DraStic, DSperate')
        self.assertEqual(r['GB'][2:4],('Gambatte, Gambatte Dual','✓✓'))
        self.assertEqual(r['GBA'][2:4],('gpSP (Wireless Adapter, AW/Gen 3 Pkmn), mGBA Dual','✓✓'))
        self.assertIsNone(actions['FC']);self.assertIsNone(actions['NDS'])
        self.assertEqual(actions['GB'],'CHANGE')

    def test_core_names_follow_engine_metadata(self):
        rows,_=ui.core_rows('core\tbase\tSFC\tSFC\tready\t0\ncore_hint\tSFC\tSupafaust')
        self.assertEqual(rows[0][2],'Supafaust')

    def test_hint_order(self):
        self.assertEqual(footer.hints('A SELECT    B BACK'),[(Button.B,'BACK'),(Button.A,'SELECT')])

    def test_active_footer_keeps_end_session_clear_of_index_on_small_screen(self):
        drawn=[]
        class Font(Enum):
            DESCRIPTIVE_LIST_TITLE=16
            DESCRIPTIVE_LIST_DESCRIPTION=12
        display=types.SimpleNamespace(
            bottom_bar=types.SimpleNamespace(get_bottom_bar_height=lambda:40),
            get_image_dimensions=lambda path:(26,26),
            get_text_dimensions=lambda purpose,label:(purpose.value*len(label),26),
            render_image=lambda *args:None,
            render_text=lambda label,x,y,color,purpose,mode:drawn.append((label,x,purpose.value*len(label))))
        modules={
            'devices.device':types.SimpleNamespace(Device=types.SimpleNamespace(get_device=lambda:types.SimpleNamespace(screen_width=lambda:640,screen_height=lambda:480))),
            'display.display':types.SimpleNamespace(Display=display),
            'display.font_purpose':types.SimpleNamespace(FontPurpose=Font),
            'display.render_mode':types.SimpleNamespace(RenderMode=types.SimpleNamespace(MIDDLE_LEFT_ALIGNED=0)),
            'themes.theme':types.SimpleNamespace(Theme=types.SimpleNamespace(text_color=lambda purpose:0)),
        }
        with patch.dict(sys.modules,modules),patch.object(footer.glyphs,'glyph',return_value=Path('badge.png')):
            footer.render([(Button.B,'BACK'),(Button.A,'CHANGE')],True)
        self.assertEqual([row[0] for row in drawn],['BACK','CHANGE','END SESSION'])
        for first,second in zip(drawn,drawn[1:]):self.assertLessEqual(first[1]+first[2],second[1]-26)
        self.assertLessEqual(drawn[-1][1]+drawn[-1][2],640-12*len('000/0000'))

if __name__=='__main__':unittest.main()
