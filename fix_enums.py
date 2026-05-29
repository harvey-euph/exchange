import os
import re

def fix_enum_file(file_path):
    with open(file_path, 'r') as f:
        lines = f.readlines()
    
    unique_lines = []
    seen = set()
    for line in lines:
        if line.strip() == "":
            unique_lines.append(line)
            continue
        if line not in seen:
            unique_lines.append(line)
            seen.add(line)
    
    with open(file_path, 'w') as f:
        f.writelines(unique_lines)

fbs_dir = '/home/harvey/exchange-web/src/fbs/exchange'
enum_files = ['order-action.ts', 'reject-code.ts', 'order-type.ts', 'side.ts', 'exec-type.ts']

for f in enum_files:
    fix_enum_file(os.path.join(fbs_dir, f))
