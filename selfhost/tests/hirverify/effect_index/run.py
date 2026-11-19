#!/usr/bin/env python3
"""Exercise the production source-node type index and HIR rejection checks."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo-root', type=Path, default=HERE.parents[3])
    parser.add_argument('--source-root', type=Path, help='directory containing wzcc packages')
    parser.add_argument('--stdlib-root', type=Path)
    parser.add_argument('--woozyc', type=Path, required=True)
    parser.add_argument('--toolchain-root', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--program-timeout', type=float, default=30)
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    source = (args.source_root or repo / 'selfhost/src/wzcc').resolve()
    stdlib = (args.stdlib_root or repo / 'stdlib').resolve()
    compiler = args.woozyc.resolve()
    work = args.work_dir.resolve()
    spec = importlib.util.spec_from_file_location('hirverify_runner', repo / 'selfhost/tests/hirverify/run.py')
    base = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(base)
    base.SELFHOST = source
    base.STDLIB = stdlib
    base.STD_PACKAGES = (*base.STD_PACKAGES, 'hash', 'map')
    work.mkdir(parents=True, exist_ok=False)
    report_path = work / 'report.json'
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    effect_source = source / 'hirverify/flow/effectsrc.wz'
    report = {'status': 'running', 'source_root': str(source), 'stdlib_root': str(stdlib),
              'compiler': str(compiler), 'compiler_sha256': sha(compiler),
              'effectsrc_sha256': sha(effect_source), 'commands': []}

    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')

    def run(label, argv, timeout):
        result = base.command([str(x) for x in argv], label, work, timeout, env)
        report['commands'].append(json.loads((work / (label + '.json')).read_text()))
        save()
        base.require_empty_stderr(result, label)
        return result

    try:
        env = base.environment(args.toolchain_root.resolve(), work)
        fixtures = repo / 'selfhost/tests/hirverify'
        index_project = work / 'index-project'
        base.stage_project(index_project, HERE / 'driver.wz', fixtures / 'lang.toml', generated=True)
        shutil.copyfile(HERE / 'index_test.wz', index_project / 'src/wzcc/hirverify/flow/index_test.wz')
        hir_project = work / 'hir-project'
        base.stage_project(hir_project, fixtures / 'src/main.wz', fixtures / 'lang.toml')
        shutil.copyfile(HERE / 'effect_control.wz', hir_project / 'src/effect_control.wz')
        shutil.copyfile(HERE / 'index_test.wz', hir_project / 'src/wzcc/hirverify/flow/index_test.wz')
        fixes = hir_project / 'src/fixes.wz'
        content = fixes.read_text()
        anchor = '    io.println("SUMMARY mode=" + mode + " fixture=" + fixture +'
        if content.count(anchor) != 1:
            raise RuntimeError('HIR fixture entry point changed')
        fixes.write_text(content.replace(anchor,
            '    if mode == "throw" { failures += check_effect_negative(parsed); }\n' + anchor))
        staged_before = {str(project): base.snapshot_tree(project / 'src')
                         for project in (index_project, hir_project)}
        for label, project in (('index', index_project), ('hir', hir_project)):
            output = work / (label + '-build')
            built = run(label + '-build', [compiler, 'build', '--project', project / 'lang.toml',
                '--backend', 'native', '--opt', '0', '--out-dir', output, '--format', 'json'], args.timeout)
            base.compiler_ok(built, label + '-build')
            if label == 'index':
                run('index-exec', [output / 'program'], args.program_timeout)
            else:
                fixture = (fixtures / 'fixture.wz').resolve()
                unit = run('unit-exec', [output / 'program', fixture, 'unit'], args.program_timeout)
                base.check_unit(unit.stdout, fixture, 'unit')
                fixture = (fixtures / 'throw.wz').resolve()
                throw = run('throw-exec', [output / 'program', fixture, 'throw'], args.program_timeout)
                base.CHECK_LABELS['throw'] = (*base.CHECK_LABELS['throw'], 'effect-set-negative-throw', 'source-effect-negative-throw')
                base.check_five_mode(throw.stdout, 'throw', fixture)
        report['staged_changes'] = [str(project) for project in (index_project, hir_project)
            if staged_before[str(project)] != base.snapshot_tree(project / 'src')]
        report['source_after_sha256'] = sha(effect_source)
        report['compiler_after_sha256'] = sha(compiler)
        if (report['staged_changes'] or report['source_after_sha256'] != report['effectsrc_sha256']
                or report['compiler_after_sha256'] != report['compiler_sha256']):
            raise RuntimeError('source or compiler changed during the run')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        save()
    print('PASS: effect source index and HIR rejection controls')


if __name__ == '__main__':
    main()
