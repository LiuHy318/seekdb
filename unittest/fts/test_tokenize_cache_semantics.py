"""A memo hit must preserve parser-name validation across argument forms."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--observe', action='store_true')
args = parser.parse_args()
client = os.environ.get('MYSQL_CLIENT', str(Path(__file__).resolve().parents[2] /
                        'deps/3rd/u01/obclient/bin/obclient'))
command = [client, '-h127.0.0.1', '-P' + os.environ.get('SEEKDB_TEST_PORT', '10106'),
           '-uroot', '-N', '-s', '--default-character-set=utf8mb4', '--raw']
failures = []
for name in ['space', 'beng', 'ik', 'ngram']:
    for properties in ["'[]'", 'NULL']:
        # JSON_ARRAY evaluates both children on one worker. The first child
        # seeds the normalized-name memo; the second must still reject name.1
        # as a public parser name, just as it does on a cold cache.
        text = f'cacheprobe_{name}_{properties == "NULL"}'
        cold = f"SELECT JSON_ARRAY(TOKENIZE('{text}','{name}.1'));"
        warm = (f"SELECT JSON_ARRAY(TOKENIZE('{text}','{name}',{properties}),"
                f"TOKENIZE('{text}','{name}.1'));")
        results = []
        for sql in [cold, warm]:
            result = subprocess.run(command + ['-e', sql], text=True, capture_output=True,
                                    timeout=30)
            error = re.search(r'ERROR (\d+)', result.stderr)
            results.append(['error', error.group(1)] if error else
                           ['success', result.stdout.strip()])
        print(name, properties, results, flush=True)
        if results[0][0] != 'error' or results[1] != results[0]:
            failures.append([name, properties, results])
if failures and not args.observe:
    raise AssertionError(f'cache bypasses validation: {failures}')
print(f'Parser validation: {8 - len(failures)}/8 consistent', flush=True)

# Exercise successful memo hits in both raw and normalized argument domains.
for name in ['space', 'beng', 'ik', 'ngram']:
    for option in ['', ", '[]'", ', NULL', ", '[{\"output\":\"all\"}]'"]:
        call = f"TOKENIZE('Alpha Alpha 数据库 123','{name}'{option})"
        result = subprocess.run(command + ['-e', f'SELECT JSON_ARRAY({call},{call});'],
                                text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr
        pair = json.loads(result.stdout)
        def canonical(value):
            if isinstance(value, list):
                return sorted((canonical(v) for v in value), key=lambda v: json.dumps(v, sort_keys=True))
            if isinstance(value, dict):
                return {k: canonical(v) for k, v in sorted(value.items())}
            return value
        assert len(pair) == 2 and canonical(pair[0]) == canonical(pair[1]), pair
print('Successful memo hits: 16/16 consistent', flush=True)
