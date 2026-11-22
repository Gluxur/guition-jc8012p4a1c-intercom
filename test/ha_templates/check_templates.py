# Checks the Jinja response templates the firmware sends to Home Assistant
# (main/net/weather.cpp and main/net/gcal.cpp, build_template()).
#
# HA renders them in a sandboxed Jinja environment with StrictUndefined, then
# turns the text back into a Python value with ast.literal_eval and sends it
# as JSON. This script does the same with fake action responses, so a typo in
# a template fails here instead of silently on a wall-mounted board.
#
#   pip install jinja2
#   python test/ha_templates/check_templates.py .
import ast, json, re, sys
from jinja2.sandbox import ImmutableSandboxedEnvironment
from jinja2 import StrictUndefined

def c_template(path):
    src = open(path, encoding='utf-8').read()
    # build_template(const char *entity, char *out, size_t cap): the format
    # string runs from snprintf's `cap,` to the `entity)` argument.
    m = re.search(r'static void build_template\(const char \*entity, char \*out, size_t cap\) \{\s*snprintf\(out, cap,(.*?),\s*entity\);', src, re.S)
    if m is None:
        sys.exit(path + ': build_template() not found: update this parser with its new signature')
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    t = ''.join(parts)
    return t

env = ImmutableSandboxedEnvironment(undefined=StrictUndefined)
env.globals['states'] = lambda e: 'partlycloudy'
env.globals['state_attr'] = lambda e, a: {'temperature': 12.5, 'humidity': 80}.get(a)

base = sys.argv[1]
w = c_template(base + '/main/net/weather.cpp') % 'weather.forecast_home'
print('WEATHER TEMPLATE:', w)
resp = {'weather.forecast_home': {'forecast': [
    {'datetime': '2026-11-21T11:00:00+00:00', 'condition': 'rainy', 'temperature': 9.1, 'templow': 4.0, 'precipitation': 2.1}
    for _ in range(10)]}}
out = env.from_string(w).render(response=resp)
val = ast.literal_eval(out)
print(json.dumps({'response': val})[:300])
assert len(val['forecast']) == 7 and val['current']['attributes']['pressure'] is None

g = c_template(base + '/main/net/gcal.cpp') % 'calendar.home'
print('GCAL TEMPLATE:', g)
resp = {'calendar.home': {'events': [
    {'start': '2026-11-21T10:00:00+01:00', 'end': '2026-11-21T11:00:00+01:00', 'summary': 'Dentiste', 'description': 'x'},
    {'start': '2026-11-22', 'end': '2026-11-23', 'summary': "L'anniv"},
    {'start': '2026-11-24', 'end': '2026-11-25'},
]}}
out = env.from_string(g).render(response=resp)
val = ast.literal_eval(out)
print(json.dumps({'response': val}))
assert val[0]['start'] == {'dateTime': '2026-11-21T10:00:00+01:00'}
assert val[1]['start'] == {'date': '2026-11-22'} and val[2]['summary'] == ''
print('OK')
