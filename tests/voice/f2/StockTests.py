"""Real bank verification and fail-closed tamper checks; no synthesis/playback."""
import importlib.util,json,shutil,sys,tempfile
from pathlib import Path
ROOT=Path(sys.argv[1]);spec=importlib.util.spec_from_file_location('embed',ROOT/'scripts/embed-f2-stock.py');embed=importlib.util.module_from_spec(spec);spec.loader.exec_module(embed)
with tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp);bank=root/'bank';shutil.copytree(ROOT/'assets/voice/f2',bank)
    output=root/'out.h';embed.generate(bank,output);assert output.stat().st_size>10000
    original=json.loads((bank/'manifest.json').read_text());row=original['clips'][0];p=bank/row['file'];data=p.read_bytes()
    def rejected():
        try:embed.generate(bank,output)
        except (AssertionError,OSError,ValueError):return
        raise AssertionError('tampered bank accepted')
    p.write_bytes(data[:-1]);rejected();p.write_bytes(data)
    changed=bytearray(data);changed[-1]^=1;p.write_bytes(changed);rejected();p.unlink()
    rejected();outside=root/'audio.wav';outside.write_bytes(data);p.symlink_to(outside);rejected();p.unlink();p.write_bytes(data)
    for mutate in [lambda m:m.update(profile='other voice'),lambda m:m['clips'][0].update(file='../audio.wav'),lambda m:m['clips'][0].update(text=m['clips'][1]['text'])]:
        m=json.loads(json.dumps(original));mutate(m);(bank/'manifest.json').write_text(json.dumps(m));rejected()
    (bank/'manifest.json').write_text(json.dumps(original));embed.generate(bank,output)
print('PASS pinned bank, corruption, truncation, missing file, symlink, profile, traversal, duplicate text, retry')
