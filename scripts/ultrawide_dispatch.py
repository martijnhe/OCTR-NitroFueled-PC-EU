"""Select one ultrawide implementation. Credits: tcosi v1.0.

v0.3 native: the aspect is driven by bbport.ini `aspect_ratio` (Settings > Display,
16:9 | 21:9 | 32:9) as a single dropdown, never by the Patches tab. patches.json
toggles are only a legacy fallback for installs whose bbport.ini has no
aspect_ratio key yet; once the key exists it owns the selection completely.
"""
from pathlib import Path
import importlib

IMPLEMENTATIONS = {
 '21:9 UltraWide': 'ultrawide_21x9',
 '32:9 Super UltraWide': 'ultrawide_32x9',
}

ASPECT_TO_MODULE = {
 '16:9': None,
 '16x9': None,
 '': None,
 '21:9': 'ultrawide_21x9',
 '21x9': 'ultrawide_21x9',
 '32:9': 'ultrawide_32x9',
 '32x9': 'ultrawide_32x9',
}

ULTRAWIDE_KEYS = {
 'CTR-UltraWide21x9-v1.0.xml/21:9 UltraWide',
 'CTR-SuperUltraWide32x9-v1.0.xml/32:9 Super UltraWide',
}

def aspect_setting(config_path):
 """bbport.ini aspect_ratio, normalized to 16:9 | 21:9 | 32:9 (default 16:9)."""
 try:
  from patches import read_settings
  value = (read_settings(Path(config_path)) or {}).get('aspect_ratio', '16:9')
 except Exception:
  value = '16:9'
 value = str(value or '16:9').strip()
 if value in ASPECT_TO_MODULE:
  return '16:9' if value in ('', '16x9') else value.replace('x', ':')
 return '16:9'

def has_aspect_key(config_path):
 """True once bbport.ini carries aspect_ratio (fresh v0.3 defaults do)."""
 try:
  from patches import read_settings
  return 'aspect_ratio' in (read_settings(Path(config_path)) or {})
 except Exception:
  return False

def choose(selected):
 active=[IMPLEMENTATIONS[m.get('Name')] for _,_,m in selected if m.get('Name') in IMPLEMENTATIONS]
 if len(active)>1:raise RuntimeError('Enable only one ultrawide patch: 21:9 or 32:9.')
 return active[0] if active else None

def prepare(port,data,env,game,config):
 from patches import external_patches,external_selection,read_settings
 if has_aspect_key(Path(config)):
  # New world: aspect_ratio owns the selection; Patches-tab toggles are ignored.
  aspect = aspect_setting(Path(config))
  if aspect in ('21:9', '32:9'):
   name = ASPECT_TO_MODULE[aspect]
  else:
   # 16:9 with the key present means off, even with a stale patches.json toggle.
   name=next((n for n in IMPLEMENTATIONS.values() if (Path(__file__).parent/(n+'.py')).exists()),None)
  if name is None:raise RuntimeError('Ultrawide helper files are missing; reinstall the package.')
  implementation=importlib.import_module(name)
  mode=implementation.prepare(port,data,env,game,config)
  mode.implementation=implementation
  return mode
 aspect = aspect_setting(Path(config))
 if aspect in ('21:9', '32:9'):
  name = ASPECT_TO_MODULE[aspect]
  if not (Path(__file__).parent/(name+'.py')).exists():
   raise RuntimeError('Ultrawide helper files are missing; reinstall the package.')
  implementation=importlib.import_module(name)
  mode=implementation.prepare(port,data,env,game,config)
  mode.implementation=implementation
  return mode
 # No aspect_ratio key yet (pre-v0.3 install): legacy patches.json toggle applies.
 directory=Path(env.get('BB_PATCHES_DIR',Path(port)/'patches'))
 selected=external_selection(external_patches(directory),Path(env.get('BB_PATCHES_CONFIG',Path(data)/'patches.json')),read_settings(Path(config)))
 name=choose(selected)
 if name is None:
  name=next((n for n in IMPLEMENTATIONS.values() if (Path(__file__).parent/(n+'.py')).exists()),None)
 if name is None:raise RuntimeError('Ultrawide helper files are missing; reinstall the package.')
 implementation=importlib.import_module(name)
 mode=implementation.prepare(port,data,env,game,config)
 mode.implementation=implementation
 return mode

def refresh(mode,env,config):
 return mode.implementation.refresh(mode,env,config)

def shaders(mode,user_dir,out):
 return mode.implementation.shaders(mode,user_dir,out)
