#!/usr/bin/env python3
import json

with open('promb_params.json') as f:
    content = f.read()
    lines = content.strip().split('\n')
    for line in lines:
        data = json.loads(line)
        if 'result' in data and isinstance(data['result'], list):
            print('Current Pro-MB Settings (from MCP read):')
            print('=' * 60)
            for param in data['result'][:88]:
                if 'State' in param['name'] or 'Threshold' in param['name'] or 'Ratio' in param['name'] or 'Crossover' in param['name']:
                    print(f"{param['id']:3d}: {param['name']:40s} = {param['value']}")
