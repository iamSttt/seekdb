import subprocess,os,sys,time
from pathlib import Path
w=Path(__file__).resolve().parent;repo=next((w/'source').iterdir());env=os.environ.copy();env.update(CARGO_NET_OFFLINE='true',CMAKE_BUILD_PARALLEL_LEVEL='24')
cmds=[['bash','tools/hipvs_bridge/build.sh',str(w/'build/libseekdb_cuvs_bridge.so')],['cmake','-S','.','-B','build_release','-G','Unix Makefiles','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DOB_ENABLE_UNITY=ON','-DOB_USE_LLD=ON','-DOB_BUILD_CUVS=ON','-DOB_BUILD_CUVS_TRACE=OFF','-DCUVS_BRIDGE_LIB='+str(w/'build/libseekdb_cuvs_bridge.so')],['cmake','--build','build_release','--target','seekdb']]
for cmd in cmds:
 print('COMMAND',cmd,flush=True);t=time.monotonic();p=subprocess.run(cmd,cwd=repo,env=env);print('EXIT',p.returncode,'SECONDS',time.monotonic()-t,flush=True)
 if p.returncode: sys.exit(p.returncode)
print('FULL BUILD PASS',flush=True)
