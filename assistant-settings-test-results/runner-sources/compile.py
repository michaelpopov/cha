import pathlib,subprocess,shlex
repo=pathlib.Path('/Users/mpopov/projects/cha');build=repo/'build/ninja';run=pathlib.Path('/private/tmp/cha-ass-settings-run')
lines=subprocess.check_output(['ninja','-t','commands','cha_app_tests'],cwd=build,text=True).splitlines()
a=shlex.split(next(s for s in lines if '-c /Users/mpopov/projects/cha/tests/app/unit_media_operations.cpp' in s))
a[a.index('-o')+1]=str(run/'fixture.o');a[a.index('-MF')+1]=str(run/'fixture.d');a[-1]=str(run/'fixture.cpp');subprocess.run(a,cwd=build,check=True)
a=shlex.split(next(s for s in reversed(lines) if '-o cha_app_tests ' in s));start=next(i for i,s in enumerate(a) if s.endswith('c++') or s.endswith('clang++'));end=a.index('&&',start) if '&&' in a[start:] else len(a);a=a[start:end];a[a.index('-o')+1]=str(run/'fixture');a=[x for x in a if not (x.endswith('.cpp.o') and 'unit_media_operations' not in x and 'test_workspace' not in x)];a=[str(run/'fixture.o') if 'unit_media_operations.cpp.o' in x else x for x in a];subprocess.run(a,cwd=build,check=True)
