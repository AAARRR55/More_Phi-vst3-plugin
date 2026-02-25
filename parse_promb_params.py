#!/usr/bin/env python3
import json

with open('promb_params.json') as f:
    content = f.read()
    lines = content.strip().split('\n')
    for line in lines:
        data = json.loads(line)
        if 'result' in data and isinstance(data['result'], list):
            print('Pro-MB Parameters:')
            print('=' * 60)
            for param in data['result']:
                print(f"{param['id']:3d}: {param['name']:40s} = {param['value']}")
            print(f"\nTotal: {len(data['result'])} parameters")
