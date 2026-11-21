#!/usr/bin/env python3
"""Offline effective receiver plan; board overlay must explicitly select PHY/DMA.
No hardware access. Never infer C-/D-PHY from the generic phy-names string.
"""
import argparse
import json
from pathlib import Path


def receiver_plan(inventory, instance, module_name):
    base, overlay = inventory['base_nodes'], inventory['overlay_nodes']
    roots = [p for p in base if p.startswith('/is_sensor'+str(instance)+'@') and p.count('/') == 1]
    if len(roots) != 1:
        raise ValueError('missing/ambiguous base receiver')
    root = roots[0]
    fragments = [p for p, v in overlay.items() if v.get('target') ==
                 [{'symbol': 'is_sensor'+str(instance), 'base_path': root}]]
    if len(fragments) != 1:
        raise ValueError('explicit board receiver override required')
    prefix = fragments[0]+'/__overlay__'
    settings = dict(base[root]);settings.update(overlay[prefix])
    if settings.get('use_cphy') != [1]:
        raise ValueError('expected explicit C-PHY board configuration')
    channels = settings.get('dma_ch', [])
    if not channels or len(set(channels)) != 1:
        raise ValueError('ambiguous DMA engine')
    dma = channels[0]
    phy = settings['phys'][0]
    if not isinstance(phy, dict) or not phy.get('base_path'):
        raise ValueError('unresolved PHY reference')
    phy_node = base[phy['base_path']]
    module = [p for p in overlay if p.rsplit('/', 1)[-1].lower().startswith('is_sensor_'+module_name+'@')]
    if len(module) != 1:
        raise ValueError('missing/ambiguous module')
    mode0 = overlay[module[0]+'/modes/mode0']
    common = mode0['common']
    if len(common) != 10 or common[5] != 2:
        raise ValueError('expected three C-PHY trios')
    routes = {}
    # Effective child routes retain base children unless a board override replaces them.
    for p, values in base.items():
        if p.startswith(root+'/sensor'+str(instance)+'-ch-mode'):
            routes[p.rsplit('/', 1)[-1]] = dict(values)
    for p, values in overlay.items():
        if p.startswith(prefix+'/sensor'+str(instance)+'-ch-mode'):
            routes.setdefault(p.rsplit('/', 1)[-1], {}).update(values)
    if not routes:
        raise ValueError('no DMA route')
    normalized = []
    for name, route in sorted(routes.items()):
        registers = route['reg']
        if len(registers) < 4 or registers[0] != 0x170c1000+dma*0x1000 or registers[2] != registers[0]+0x400:
            raise ValueError('DMA engine disagrees with effective register route')
        normalized.append({'name': name, 'vc0_base': registers[0],
                           'common_base': registers[2], 'mux_reg': route['mux_reg']})
    return {'sensor': module_name, 'instance': instance,
            'csis_base': settings['reg'][1], 'phy_base': phy_node['reg'][1],
            'phy_reset_bit_index': phy_node['reset'][0], 'phy_mode': 'C-PHY',
            'trios': common[5]+1, 'dma_engine': dma, 'routes': normalized,
            'first_mode': {'width': common[0], 'height': common[1], 'fps_label': common[2],
                           'mipi_speed_label': common[6], 'vc0': mode0['vc0'], 'vc1': mode0['vc1']},
            'verified_on_device': False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventory',type=Path);args=parser.parse_args()
    captures=json.loads(args.inventory.read_text())
    plans=[[receiver_plan(i,4,'2la'),receiver_plan(i,2,'gw2')] for i in captures]
    if any(p!=plans[0] for p in plans[1:]):
        raise ValueError('effective receiver plans differ between captured overlays')
    print(json.dumps({'schema':1,'overlay_sha256':[i['overlay_sha256'] for i in captures],
                      'plans':plans[0]},indent=2,sort_keys=True))


if __name__=='__main__':main()
