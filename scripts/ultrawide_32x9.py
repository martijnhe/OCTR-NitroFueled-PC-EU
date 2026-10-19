"""Super UltraWide v1.0 launcher integration (v0.3 Windows).
Original game archives and launcher settings are never edited by this module.

v0.3 native: enabled by bbport.ini aspect_ratio=32:9 (Settings > Display).
A legacy patches.json toggle still enables it when aspect_ratio is 16:9/unset.
"""
from pathlib import Path
import hashlib,json,os,shutil,struct,uuid,copy
import xml.etree.ElementTree as ET
from types import SimpleNamespace
from ultrawide_32x9_archive import Archive
from ultrawide_32x9_shader import centered_shader,SIGNATURES,known_ultrawide_shader,require_warm_cache

DATA=Path(__file__).resolve().parent/'ultrawide_32x9_data'
WORK=DATA  # the manifest in use and the prepared view (US: as before; EU: DATA/'eu')
MANIFEST=json.loads((DATA/'manifest.json').read_text(encoding='utf-8'))
def select_data():
 global WORK,MANIFEST
 if TITLE=='CUSA13795':
  WORK=DATA
 else:
  import ultrawide_eu
  WORK=DATA/'eu';WORK.mkdir(exist_ok=True)
  source=DATA/ultrawide_eu.manifest_name('manifest.json')
  if not (WORK/'manifest.json').exists() or (WORK/'manifest.json').read_bytes()!=source.read_bytes():atomic(WORK/'manifest.json',source.read_bytes())
 MANIFEST=json.loads((WORK/'manifest.json').read_text(encoding='utf-8'))
KEY=MANIFEST['key']
# The game's edition (set_title): the European one (CUSA14876) uses the ported data of
# ultrawide_eu.py and its own work folder, so a US view is never mistaken for it.
TITLE='CUSA13795'
def set_title(game):
 global TITLE
 from game_check import sfo_info
 TITLE=sfo_info(Path(game)).get('TITLE_ID') or 'CUSA13795'
 select_data()
OWN_ASPECT='32:9'
LEGACY_FILES={'CTR-Centered-HUD-720p-v3.xml','CTR-Centered-HUD-v4.xml','CTR-HUD-Rotation-Fix.xml','CTR-SuperUltrawide-2560x720.xml','CTR-SuperUltrawide-3840x1080.xml','CTR-SuperUltrawide-5120x1440.xml','HUD-Screen-Effects.xml','HUD-Clipping-Diagnostic.xml','HUD-Rotation-Diagnostic.xml','HUD-Rotation-Diagnostic-v2.xml','Movie-Aspect.xml'}

def aspect_of(config_path):
 try:
  from patches import read_settings
  value=str((read_settings(Path(config_path)) or {}).get('aspect_ratio','16:9') or '16:9').strip()
  if value in ('21x9',):return '21:9'
  if value in ('32x9',):return '32:9'
  if value in ('16x9', '', None):return '16:9'
  return value if value in ('16:9','21:9','32:9') else '16:9'
 except Exception:
  return '16:9'

def select_profile(settings):
 """v0.3 native: a 32:9 output is required (5120x1440 shape)."""
 output=settings.get('output_res','')
 import re
 match=re.fullmatch(r'(\d+)x(\d+)',output or '')
 if not match:raise RuntimeError('Select a 32:9 output resolution (for example 5120x1440) before launching this patch.')
 w,h=map(int,match.groups())
 if not h:raise RuntimeError('Invalid output resolution')
 ratio=w/h
 if abs(ratio-32/9)<0.02:return '5120'
 raise RuntimeError('This 32:9 patch requires a 5120x1440 shaped output. Select one in the launcher; do not combine it with 21:9.')

def digest(p):
 with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def stamp(p):
 s=Path(p).stat();return [s.st_size,s.st_mtime_ns]
def atomic(p,data):
 p=Path(p);p.parent.mkdir(parents=True,exist_ok=True);tmp=p.with_name(p.name+'.'+uuid.uuid4().hex+'.tmp');tmp.write_bytes(data);os.replace(tmp,p)
def json_write(p,value):atomic(p,json.dumps(value,indent=2).encode('utf-8'))
def link_tree(source,target):
 for f in source.rglob('*'):
  rel=f.relative_to(source);dest=target/rel
  if f.is_dir():dest.mkdir(parents=True,exist_ok=True)
  elif f.is_file():
   dest.parent.mkdir(parents=True,exist_ok=True)
   try:os.link(f,dest)
   except OSError:shutil.copy2(f,dest)

def patched_payload(raw,entry):
 if hashlib.sha256(raw).hexdigest()!=entry['original_sha256']:raise RuntimeError('Unrecognized original asset: '+entry['path'])
 result=bytearray(raw)
 for off,old,new in entry['runs']:
  old,new=bytes.fromhex(old),bytes.fromhex(new)
  if result[off:off+len(old)]!=old or len(old)!=len(new):raise RuntimeError('Invalid asset delta')
  result[off:off+len(old)]=new
 if hashlib.sha256(result).hexdigest()!=entry['patched_sha256']:raise RuntimeError('Asset verification failed')
 return result

def verify_assets(game):
 for item in MANIFEST['archives']:
  a=Archive(Path(game)/'archives'/item['archive'])
  try:
   for entry in item['files']:
    f=a.files[entry['index']]
    if f['path']!=entry['path'] or hashlib.sha256(a.extract(f)).hexdigest()!=entry['patched_sha256']:raise RuntimeError('Patched archive mismatch: '+entry['path'])
  finally:a.data.close();a._file.close()

def view_record(source,game):
 return dict(source=str(source.resolve()),manifest_sha256=digest(WORK/'manifest.json'),archives=[dict(archive=a['archive'],source_stamp=stamp(source/'archives'/a['archive']),view_stamp=stamp(game/'archives'/a['archive'])) for a in MANIFEST['archives']])

def view_root(source,base):
 """Where the prepared view goes: WORK (in the port folder), or next to the game when the game is
 on another drive: the view is hard links to the game's files, and links only work within one
 drive (else the whole game, 20+ GB, would be copied)."""
 try:
  same=os.stat(source).st_dev==os.stat(base).st_dev
 except OSError:
  same=True
 return base if same else Path(source).parent/(Path(source).name+'-ultrawide')/base.name

def ensure_view(source):
 source=Path(source).resolve();ROOT=view_root(source,WORK);ROOT.mkdir(parents=True,exist_ok=True);game=ROOT/'view/game';record=ROOT/'view/ready.json'
 if record.exists():
  state=json.loads(record.read_text(encoding='utf-8'))
  if state['source']!=str(source) or state['manifest_sha256']!=digest(WORK/'manifest.json'):raise RuntimeError('Super UltraWide view belongs to another game or package version; reinstall the package.')
  for a in state['archives']:
   if stamp(source/'archives'/a['archive'])!=a['source_stamp'] or stamp(game/'archives'/a['archive'])!=a['view_stamp']:raise RuntimeError('Super UltraWide source or prepared archives changed; reinstall before launching.')
  return game
 if (ROOT/'view').exists():raise RuntimeError('Incomplete Super UltraWide view; reinstall before launching.')
 required=sum(a['original_size'] for a in MANIFEST['archives'])+64*1024*1024
 if shutil.disk_usage(DATA).free<required:raise RuntimeError('Super UltraWide needs about 9 GB free for its first preparation; original game files are retained.')
 for a in MANIFEST['archives']:
  if digest(source/'archives'/a['archive'])!=a['original_sha256']:raise RuntimeError('Unsupported or modified archive: '+a['archive'])
 temporary=ROOT/('view-building-'+uuid.uuid4().hex);view=temporary/'game';view.mkdir(parents=True)
 # Partial builds are kept on failure for diagnosis; never modify source links.
 link_tree(source,view)
 for item in MANIFEST['archives']:
  p=view/'archives'/item['archive'];replacement=p.with_suffix('.uw-building')
  shutil.copy2(p,replacement);os.replace(replacement,p)
  if p.stat().st_nlink!=1:raise RuntimeError('Refusing to modify a shared archive')
  a=Archive(p);headers=[];payloads=[]
  try:
   for entry in item['files']:
    f=a.files[entry['index']];payloads.append((entry,patched_payload(a.extract(f),entry)))
   sector=a.sector;count=len(a.files)
   old_headers={e['index']:bytes(a.data[56+count*4+e['index']*16:72+count*4+e['index']*16]) for e,_ in payloads}
  finally:a.data.close();a._file.close()
  with p.open('r+b') as f:
   for entry,payload in payloads:
    f.seek(0,2);end=f.tell();offset=(end+sector-1)//sector*sector;f.write(b'\0'*(offset-end));f.write(payload)
    old=struct.unpack('<4I',old_headers[entry['index']]);headers.append((56+count*4+entry['index']*16,struct.pack('<4I',offset&0xffffffff,(old[1]&0xffffff00)|(offset>>32),len(payload),0xffffffff)))
   for off,value in headers:f.seek(off);f.write(value)
   f.flush();os.fsync(f.fileno())
 verify_assets(view);json_write(temporary/'ready.json',view_record(source,view));os.replace(temporary,ROOT/'view')
 return game

def prepare(port,data,env,game,config):
 from patches import external_patches,external_selection,read_settings
 set_title(game)
 selection_path=Path(env.get('BB_PATCHES_CONFIG',Path(data)/'patches.json'))
 patch_dir=Path(env.get('BB_PATCHES_DIR',Path(port)/'patches'))
 ini_settings=read_settings(Path(config))
 selected=external_selection(external_patches(patch_dir),selection_path,ini_settings)
 legacy_enabled=any(key==KEY for key,_,_ in selected)
 try:
  from ultrawide_dispatch import has_aspect_key
  key_present=has_aspect_key(Path(config))
 except Exception:
  key_present='aspect_ratio' in (ini_settings or {})
 aspect=aspect_of(Path(config))
 if aspect not in ('16:9','21:9','32:9'):
  aspect='16:9'
 if key_present:
  # v0.3: aspect_ratio owns it; Patches-tab toggles are ignored entirely.
  enabled=(aspect==OWN_ASPECT)
  if enabled:
   # v0.3 native: fail early on a non-32:9 output (21:9 already validates its own).
   select_profile(ini_settings)
 else:
  if aspect=='21:9':
   enabled=False
  elif aspect=='32:9':
   enabled=True
   # v0.3 native: fail early on a non-32:9 output (21:9 already validates its own).
   select_profile(ini_settings)
  else:
   enabled=legacy_enabled
   if not key_present and enabled and any('21:9' in m.get('Name','') for _,_,m in selected):raise RuntimeError('Enable only one aspect-ratio patch at a time.')
 # Freeze the checkbox for this entire process (including in-game restarts).
 # All archive, shader and executable components switch together on next launch.
 settings=json.loads(selection_path.read_text(encoding='utf-8')) if selection_path.exists() else {}
 settings['enabled']=[key for key,_,_ in selected if key.split('/',1)[0] not in LEGACY_FILES]
 settings['disabled']=list(dict.fromkeys(settings.get('disabled',[])+[key for key,_,_ in external_patches(patch_dir) if key not in settings['enabled']]))
 settings['disabled']=[key for key in settings['disabled'] if key not in settings['enabled']]
 try:
  from ultrawide_dispatch import ULTRAWIDE_KEYS
 except Exception:
  ULTRAWIDE_KEYS={KEY}
 if key_present:
  settings['enabled']=[k for k in settings['enabled'] if k not in ULTRAWIDE_KEYS]
  settings['disabled']=[k for k in settings['disabled'] if k not in ULTRAWIDE_KEYS]
  other=[k for k in ULTRAWIDE_KEYS if k!=KEY]
  settings['disabled']=list(dict.fromkeys(settings.get('disabled',[])+other))
  if enabled and KEY not in settings['enabled']:
   settings['enabled'].append(KEY)
   settings['disabled']=[k for k in settings['disabled'] if k!=KEY]
 effective=Path(data)/'out/ultrawide-32x9';effective.mkdir(parents=True,exist_ok=True)
 snapshot=effective/'patches.json';json_write(snapshot,settings);env['BB_PATCHES_CONFIG']=str(snapshot)
 source_patches=external_patches(patch_dir)
 base_selected=[x for x in selected if x[0]==KEY]
 if enabled and not base_selected:
  base_selected=[x for x in source_patches if x[0]==KEY]
  if not base_selected:raise RuntimeError('32:9 Super UltraWide patch XML is missing from patches/.')
 mode=SimpleNamespace(enabled=enabled,game=Path(game),config=str(config),render_config=str(config),selected=base_selected,patch_dir=patch_dir,effective=effective)
 if enabled:
  print('32:9 Super UltraWide: preparing game view (first run takes several minutes, ~9 GB free needed)...',flush=True)
  mode.game=ensure_view(game)
  # Preserve the selected internal and output resolutions in the runtime UI.
  # Only the compiler uses a separate base-path settings file.
  text=Path(config).read_text(encoding='utf-8') if Path(config).exists() else ''
  override={'temporal':'off','upscaler':'off','patch_pro_1080p':'0'}
  lines=[line for line in text.splitlines() if line.partition('=')[0].strip() not in override]
  lines += [f'{k}={v}' for k,v in override.items()]
  cfg=effective/'bbport.ini';atomic(cfg,('\n'.join(lines)+'\n').encode('utf-8'))
  mode.config=str(cfg);env['BB_CONFIG']=str(cfg)
  refresh(mode,env,mode.config)
 else:print('32:9 Super UltraWide: disabled; original game assets and settings',flush=True)
 return mode

def dimensions(settings):
 from patches import RENDER_RESOLUTIONS
 value=settings.get('render_res','1080')
 if value not in RENDER_RESOLUTIONS:raise RuntimeError('Unsupported internal resolution: '+str(value))
 height=min(int(value),1440)
 # Keep horizontal dimensions even; 480p is rounded to the nearest two pixels.
 width=round(height*32/9/2)*2
 return width,height

def resolution_metadata(meta,settings):
 width,height=dimensions(settings);meta=copy.deepcopy(meta);seen=set()
 for line in meta.iter('Line'):
  addr=int(line.get('Address'),0);blob=bytearray.fromhex(line.get('Value'))
  if addr in (0x17a9d61,0x17a9d76):
   assert blob[:7]==bytes.fromhex('41c78640050000' if addr==0x17a9d61 else '41c78644050000')
   struct.pack_into('<I',blob,7,width if addr==0x17a9d61 else height);seen.add(addr)
  elif addr==0x658df9:
   needle=bytes.fromhex('ba0000a044660f6eda');assert blob.count(needle)==1
   offset=blob.index(needle)+1;struct.pack_into('<f',blob,offset,width/4);seen.add(addr)
  line.set('Value',blob.hex())
 assert seen=={0x17a9d61,0x17a9d76,0x658df9}
 return meta

def refresh(mode,env,config):
 if not mode.enabled:return
 from patches import read_settings,external_patches
 settings=read_settings(Path(config))
 # v0.3 native: the output must stay 32:9 for the whole session.
 select_profile(settings)
 mode.width,mode.height=dimensions(settings)
 if int(settings.get('render_res','1080'))>mode.height:
  print('32:9 Super UltraWide: selected internal resolution capped at 5120x1440',flush=True)
  text=Path(config).read_text(encoding='utf-8')
  lines=[line for line in text.splitlines() if line.partition('=')[0].strip()!='render_res']
  atomic(config,('\n'.join(lines+['render_res=1440'])+'\n').encode('utf-8'))
  settings['render_res']='1440'
 source=external_patches(mode.patch_dir);original=next(x for x in source if x[0]==KEY)
 meta=resolution_metadata(original[2],settings)
 if TITLE!='CUSA13795':
  import ultrawide_eu
  meta=ultrawide_eu.port_meta(meta);print(f'{OWN_ASPECT} ultrawide: executable patches moved for {TITLE}',flush=True)
 mode.selected=[(KEY,original[1],meta)]
 generated=mode.effective/'patches';generated.mkdir(exist_ok=True)
 # Keep other patch files available; selection remains frozen for this launch.
 for p in mode.patch_dir.glob('*.xml'):
  if p.name!=KEY.split('/',1)[0]:atomic(generated/p.name,p.read_bytes())
 root=ET.Element('Patch');ids=ET.SubElement(root,'TitleID');ET.SubElement(ids,'ID').text=TITLE;root.append(meta);ET.indent(root)
 atomic(generated/KEY.split('/',1)[0],ET.tostring(root,encoding='utf-8',xml_declaration=True))
 env['BB_PATCHES_DIR']=str(generated);env['BB_NEO']='0'
 text=Path(config).read_text(encoding='utf-8');lines=[line for line in text.splitlines() if line.partition('=')[0].strip() not in {'render_res','patch_pro_1080p','temporal','upscaler'}]
 lines+=['render_res=1080','patch_pro_1080p=0','temporal=off','upscaler=off']
 compile_config=mode.effective/'render-compiler.ini';atomic(compile_config,('\n'.join(lines)+'\n').encode('utf-8'));mode.render_config=str(compile_config)
 print(f'32:9 Super UltraWide — by tcosi v1.0: {mode.width}x{mode.height} internal; selected output unchanged',flush=True)

def shaders(mode,user_dir,out):
 if mode.enabled:
  from patches import compile_external,eboot_segments
  elf=(Path(out)/'eboot.elf').read_bytes();writes=compile_external(mode.selected,eboot_segments(elf),elf)
  if len(writes)!=sum(len(list(m.iter('Line'))) for _,_,m in mode.selected):raise RuntimeError('Super UltraWide executable signatures do not match; launch stopped.')
 cache=Path(user_dir)/'cache'/TITLE;operations=[]
 if mode.enabled:require_warm_cache(cache)
 for shader,signature in SIGNATURES.items():
  name=f'0x00000000{shader}_0.spv';original=(DATA/'shaders'/name).read_bytes()
  if hashlib.sha256(original).hexdigest()!=signature:raise RuntimeError('Super UltraWide shader package is damaged')
  corrected=centered_shader(original);p=cache/name
  if p.exists():
   current=p.read_bytes()
   if not known_ultrawide_shader(current,original):raise RuntimeError('Another modification uses the HUD shader '+name+'; launch stopped to preserve it.')
   desired=corrected if mode.enabled else original
   if current!=desired:operations.append((p,desired))
  elif mode.enabled:operations.append((p,corrected))
 # Validate both before making any writes. No stale correction remains when off.
 for p,value in operations:atomic(p,value)
