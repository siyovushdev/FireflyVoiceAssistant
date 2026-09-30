from pathlib import Path
import subprocess,time,json,signal,sys,datetime,argparse
parser=argparse.ArgumentParser(description='Exclusive monitored on-board acceptance; no SIGKILL')
parser.add_argument('--prefix',default='build/dialogue-recovery/acceptance')
parser.add_argument('--fixture',help='Previously confirmed private TEST configuration only')
args=parser.parse_args()
prefix=Path(args.prefix);prefix.parent.mkdir(parents=True,exist_ok=True)
if any(Path(str(prefix)+suffix).exists() for suffix in ('.log','.json','-memory.jsonl')):
 raise SystemExit('Refusing to overwrite an existing report; choose a new --prefix')
available=int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:')))
if available<6*1024*1024:raise SystemExit('At least 6 GiB MemAvailable required before model admission')
for d in Path('/proc').iterdir():
 if not d.name.isdigit():continue
 try:
  if 'librkllmrt.so' in (d/'maps').read_text():raise SystemExit('REFUSE: existing RKLLM mapping '+d.name)
 except OSError:pass
log=open(str(prefix)+'.log','w')
command=['build/src/admin/dialogue_acceptance',str(prefix)+'.json']
if args.fixture:command.append(args.fixture)
p=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT)
start=time.monotonic();stop_at=None
with open(str(prefix)+'-memory.jsonl','w',buffering=1) as out:
 while p.poll() is None:
  mem={a:int(b.split()[0]) for a,b in (l.split(':',1) for l in Path('/proc/meminfo').read_text().splitlines())}
  record={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'pid':p.pid,'seconds':round(time.monotonic()-start,1),'available_kib':mem['MemAvailable']}
  try:
   sm=(Path('/proc')/str(p.pid)/'smaps_rollup').read_text()
   record.update({a:int(b.split()[0]) for a,b in (l.split(':',1) for l in sm.splitlines() if l.startswith(('Rss:','Pss:')))})
  except OSError:pass
  if stop_at is None and (mem['MemAvailable']<1048576 or time.monotonic()-start>1200):
   record['stop_reason']='memory_reserve' if mem['MemAvailable']<1048576 else 'wall_deadline'
   p.send_signal(signal.SIGTERM);stop_at=time.monotonic()
  out.write(json.dumps(record)+'\n')
  if stop_at and time.monotonic()-stop_at>120:
   out.write(json.dumps({'blocked':'graceful shutdown not completed; no SIGKILL sent','pid':p.pid})+'\n')
   print('BLOCKED child still running',p.pid,flush=True);sys.exit(3)
  time.sleep(1)
print('exit',p.returncode,'elapsed',time.monotonic()-start,flush=True)

sys.exit(p.returncode)
