"""Validate recorded REAL model acceptance, never synthesize a model result."""
import json,sys
from pathlib import Path
report=json.loads(Path(sys.argv[1]).read_text())
assert report.get('admin_stt_60s',{}).get('ok') is True
assert report.get('complete') is True,report.get('error','Incomplete acceptance')
cases=report['cases']
assert len(cases) in (4,6)
if len(cases)==6:
    first,second=cases[:2]
    config=first['state']['configuration']
    assert len(config['rooms'])==1 and 'детск' in config['rooms'][0]['name'].lower()
    assert len(config['devices'])==2
    names=[d['name'].lower() for d in config['devices']]
    assert any('телев' in name for name in names) and any('свет' in name for name in names)
    for case in (first,second):
        assert case['state']['status']=='editing' and not case['state']['questions']
        for d in case['state']['configuration']['devices']:
            assert len(d['action_ids'])==2 and len(d['state'])==1,(d['name'],'unexpected capability')
    names=[d['name'].lower() for d in second['state']['configuration']['devices']]
    assert len(names)==3 and any('основн' in n for n in names) and any('ночник' in n for n in names)
    assert report['first_apply']['status']=='applied'
renamed,ambiguous,clarified,long=cases[-4:]
assert renamed['state']['status']=='editing'
assert any('экран' in d['name'].lower() for d in renamed['state']['configuration']['devices'])
assert ambiguous['state']['status']=='clarification_required' and ambiguous['state']['questions']
assert clarified['state']['status']=='editing' and not clarified['state']['questions']
assert any('телев' in d['name'].lower() for d in clarified['state']['configuration']['devices'])
assert long['state']['status']=='editing' and not long['state']['questions']
config=long['state']['configuration']
assert len(config['rooms'])==3 and len(config['devices'])==4
rooms=[r['name'].lower() for r in config['rooms']]
assert any('кабинет' in n for n in rooms) and any('кладов' in n for n in rooms)
names=[d['name'].lower() for d in config['devices']]
assert any('ламп' in n for n in names) and not any('вентилятор' in n for n in names)
assert report['cancel']['status']=='cancelled_draft_saved'
assert report['runtime_compact']['raw'].strip()=='[40]'
assert report['stt_replay']['ok'] and 'сорок' in report['stt_replay']['text']
print('PASS real report: named graph, exact power-only capabilities, same-draft followup, clarification, final exclusion, Apply/cancel/runtime compact/STT')
