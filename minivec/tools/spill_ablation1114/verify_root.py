#!/usr/bin/env python3
"""Bind a newly built pinned root image ELF to the frozen ablation identity."""
import argparse
import json
from pathlib import Path
from run_ablation import PLAN,require,sha,save

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role',choices=('baseline','candidate'),required=True)
    for key in ('context','binary','image-inspect','output'):parser.add_argument('--'+key,type=Path,required=True)
    args=parser.parse_args();expected=PLAN['roles'][args.role]
    actual={p.relative_to(args.context).as_posix():sha(p) for p in args.context.rglob('*') if p.is_file()}
    require(actual==expected['build_inputs'],'archive source inventory differs from pinned Git blobs')
    digest=sha(args.binary);require(digest==expected['expected_binary_sha256'],'new root ELF does not match previously validated source pairing')
    image=json.loads(args.image_inspect.read_bytes());require(len(image)==1,'expected one actual image')
    save(args.output,{'status':'verified_pinned_root_build','source_commit':expected['source_commit'],
        'binary_sha256':digest,'build_inputs':actual,'image_id':image[0]['Id'],
        'image_inspect_sha256':sha(args.image_inspect),'frozen_plan_sha256':sha(Path(__file__).with_name('plan.json')),
        'scope':'Actual ELF freshly extracted from the original pinned source root Docker build; not a formal-score or recovered-official-executable claim.'})
if __name__=='__main__':main()
