"""Real extractor processes: UI exit/crash, reattachment, locks and receipts."""
from pathlib import Path
import hashlib, json, os, signal, subprocess, tempfile, time, zipfile
from rar_worker import build_host
from test_archives import ROOT, encrypted_rar5, DATA

def wait_for(check, description, timeout=15):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        result=check()
        if result:return result
        time.sleep(.05)
    raise AssertionError('Timed out: '+description)

def main():
    with tempfile.TemporaryDirectory() as temporary:
        root=Path(temporary)
        worker=build_host(root/'rar-extract')
        client=root/'archive-client'
        sources=['tools/tests/archive_client.cpp','native/shared/archives.cpp','native/shared/archive_preflight.cpp',
                 'native/shared/rar5_password.cpp','native/shared/archive_writer.cpp','native/desktop/rar_worker_process.cpp']
        subprocess.run(['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT/'native/shared'),
                        *[str(ROOT/p) for p in sources],'-larchive','-llzma','-lcrypto','-lz','-o',str(client)],check=True)
        journal_client=root/'journal-client'
        subprocess.run(['clang++','-std=c++20','-O1','-Wall','-Wextra','-Werror','-pthread',
                        '-fsanitize=address,undefined',str(ROOT/'tools/tests/archive_journal_client.cpp'),'-lcrypto','-o',str(journal_client)],check=True)
        env={**os.environ,'ARCHIVE_DURABLE':'1','PS5_REACT_RAR_WORKER':str(worker),'ARCHIVE_TRACE':'1',
             'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1','ARCHIVE_PHASES':'1'}
        processes=[];owned_workers=set();passed=[]
        def children_of(process):
            result=set()
            # posix_spawn runs on the controller's archive thread, so Linux
            # records its child under that task rather than the main task.
            for task in Path(f'/proc/{process.pid}/task').glob('*'):
                try:result.update(int(pid) for pid in (task/'children').read_text().split())
                except FileNotFoundError:pass
            return result
        def launch(name,paths,*args,limit=2**20):
            log=root/f'{name}-{len(processes)}.stderr'
            errors=log.open('w')
            process=subprocess.Popen([str(client),str(root/name),str(limit),*[str(p) for p in paths],*args],
                                     stdout=subprocess.PIPE,stderr=errors,text=True,env=env)
            process.error_path=log;process.error_file=errors
            processes.append(process)
            return process
        def journal(name):return root/(name+'.extracting/.ps5-react-worker-journal')
        def ready(name,process):
            def check():
                owned_workers.update(children_of(process))
                path=journal(name)
                return path.exists() and '\nready\n' in path.read_text()
            wait_for(check,'persistent worker ready '+name)
            return journal(name).stat().st_ino
        def finish(process,expected='completed'):
            out,_=process.communicate(timeout=50)
            process.error_file.flush()
            err=process.error_path.read_text()
            assert process.returncode==0,(process.returncode,out,err)
            assert out.splitlines()[0]==expected,(out,err)
            assert 'AddressSanitizer' not in err and 'runtime error:' not in err,err
            return out,err
        def waiting(name):
            folder=root/(name+'-volumes');folder.mkdir()
            blobs=encrypted_rar5(DATA,password='fixture-password',volumes=4)
            paths=[folder/f'game.part{i+1}.rar' for i in range(len(blobs))]
            paths[0].write_bytes(blobs[0])
            return paths,blobs
        def supply(paths,blobs):
            for path,blob in zip(paths[1:],blobs[1:]):path.write_bytes(blob)
        try:
            direct=root/'socketless';direct.mkdir()
            identity='a'*64;generation='b'*64
            header=f'P5WK002:{identity}:1048576:{generation}\n'
            (direct/'.ps5-react-worker-journal').write_text(header)
            # Leave stdout open and completely unread, as a payload loader may
            # after the original UI exits. File reports must never block on it.
            process=subprocess.Popen([str(journal_client),str(direct),identity,generation],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
            try:assert process.wait(timeout=10)==0
            finally:
                if process.poll() is None:process.kill();process.wait()
            out,err=process.communicate()
            assert not out and not err,(out,err)
            before=(direct/'.ps5-react-worker-journal').read_bytes()
            assert before.count(b'\np 123\n')>10000 and before.endswith(b'\nok\n')
            late=subprocess.run([str(journal_client),str(direct),identity,'c'*64],capture_output=True,env=env)
            assert late.returncode==23 and (direct/'.ps5-react-worker-journal').read_bytes()==before
            passed.append('unread orphan socket cannot block reports; stale-generation worker refused without writes')

            # Kill a UI while its real encrypted RAR worker waits for volumes.
            paths,blobs=waiting('crash')
            first=launch('crash',paths,'--stream','--password','fixture-password')
            inode=ready('crash',first)
            second=launch('crash',paths,'--stream','--password','fixture-password')
            out,_=finish(second,'failed');assert 'Another application controls' in out,out
            first.kill();first.wait(timeout=5);first.stdout.close()
            # New UI can follow the worker with a smaller *remaining* byte cap.
            follower=launch('crash',paths,'--stream','--password','fixture-password',limit=1)
            time.sleep(.3)
            assert journal('crash').stat().st_ino==inode
            supply(paths,blobs)
            out,err=finish(follower)
            assert 'worker reattached' in err,err
            assert (root/'crash/game.ffpfsc').read_bytes()==DATA
            text=(root/'crash/.ps5-react-worker-journal').read_text()
            assert text.splitlines().count('ready')==1 and text.endswith('\nok\n'),text[-500:]
            passed.append('UI SIGKILL and exact worker reattachment; one decoder; saved cap; verified encrypted output')

            # Normal host cleanup detaches instead of cancelling a payload.
            paths,blobs=waiting('closed')
            first=launch('closed',paths,'--stream','--password','fixture-password','--detach-after','1200')
            ready('closed',first)
            finish(first,'detached')
            assert not (root/'closed.extracting/.ps5-react-worker-cancel').exists()
            supply(paths,blobs)
            wait_for(lambda:journal('closed').read_text().endswith('\nok\n'),'worker finishes with UI closed')
            follower=launch('closed',paths,'--password','fixture-password')
            finish(follower)
            assert (root/'closed/game.ffpfsc').read_bytes()==DATA
            passed.append('graceful UI shutdown; extraction completes while closed; completion receipt recovered')

            # Explicit pause cancels the surviving task by its durable marker.
            paths,blobs=waiting('cancel')
            first=launch('cancel',paths,'--stream','--password','fixture-password','--detach-after','1200')
            ready('cancel',first);finish(first,'detached')
            follower=launch('cancel',paths,'--stream','--password','fixture-password','--cancel-after','100')
            finish(follower,'cancelled')
            assert paths[0].read_bytes()==blobs[0]
            assert not (root/'cancel.extracting').exists()
            passed.append('explicit cancel after reattachment; downloaded volumes retained')

            # ZIP takes the same durable path, not an in-title decoder.
            archive=root/'plain.zip'
            with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as package:package.writestr('game.ffpfsc',DATA)
            process=launch('zip',[archive]);out,err=finish(process)
            assert 'worker persistent started' in err and (root/'zip/game.ffpfsc').read_bytes()==DATA
            passed.append('ZIP independent worker; real extracted bytes and receipt')

            (root/'game.ffpfsc').write_bytes(DATA)
            seven=root/'game.7z'
            subprocess.run(['bsdtar','--format=7zip','-cf',str(seven),'-C',str(root),'game.ffpfsc'],check=True)
            out,err=finish(launch('seven',[seven]))
            assert (root/'seven/game.ffpfsc').read_bytes()==DATA
            assert 'total '+str(len(DATA))+'\n' in err,err
            passed.append('7z independent worker; exact expanded byte total before extraction')

            # Old partial data must never be silently truncated during an update.
            stage=root/'legacy.extracting';stage.mkdir();(stage/'keep').write_bytes(b'partial user data')
            st=archive.stat();identity=f'{archive}|{st.st_dev}|{st.st_ino}|{st.st_size}|{st.st_mtime_ns//10**9}|{st.st_mtime_ns%10**9}\n'
            (stage/'.ps5-react-stage').write_text('P5ST001:'+hashlib.sha256(identity.encode()).hexdigest()+'\n')
            out,_=finish(launch('legacy',[archive]),'failed')
            assert 'older worker was preserved' in out and (stage/'keep').read_bytes()==b'partial user data'
            passed.append('legacy partial extraction preserved; no silent reset')

            # Tamper with a completed detached worker's file: reattachment must
            # rehash it, not publish a same-sized corrupt output.
            paths,blobs=waiting('tamper')
            first=launch('tamper',paths,'--stream','--password','fixture-password','--detach-after','1200')
            ready('tamper',first);finish(first,'detached');supply(paths,blobs)
            wait_for(lambda:journal('tamper').read_text().endswith('\nok\n'),'tamper worker complete')
            output=root/'tamper.extracting/game.ffpfsc'
            output.write_bytes(bytes(len(output.read_bytes())))
            out,_=finish(launch('tamper',paths,'--stream','--password','fixture-password'),'failed')
            assert 'SHA-256 mismatch' in out and not (root/'tamper').exists(),out
            assert paths[0].read_bytes()==blobs[0]
            passed.append('same-size recovered output corruption rejected by SHA-256')

            # A genuinely dead decoder may restart only after its kernel lease
            # is released and its journal is stale. Its partial data is parked,
            # while the already downloaded volumes are reused unchanged.
            paths,blobs=waiting('dead')
            first=launch('dead',paths,'--stream','--password','fixture-password')
            ready('dead',first)
            dead_workers=list(children_of(first))
            assert len(dead_workers)==1,dead_workers
            first.kill();first.wait(timeout=5);first.stdout.close()
            os.kill(dead_workers[0],signal.SIGKILL)
            wait_for(lambda: not Path(f'/proc/{dead_workers[0]}').exists() or
                     Path(f'/proc/{dead_workers[0]}/stat').read_text().split()[2]=='Z','owned decoder exit')
            stage=root/'dead.extracting';(stage/'keep-partial').write_bytes(b'owned partial bytes')
            os.utime(journal('dead'),(time.time()-60,time.time()-60))
            supply(paths,blobs)
            out,err=finish(launch('dead',paths,'--password','fixture-password'))
            assert 'restarting interrupted extraction' in err,err
            parked=list(root.glob('dead.extracting.interrupted-*'))
            assert len(parked)==1 and (parked[0]/'keep-partial').read_bytes()==b'owned partial bytes'
            assert (root/'dead/game.ffpfsc').read_bytes()==DATA
            assert all(path.read_bytes()==blob for path,blob in zip(paths,blobs))
            passed.append('dead worker restart requires stale journal and released lock; partial output parked; no redownload')
            print(json.dumps({'passed':len(passed),'checks':passed,'host_asan_ubsan':'clean','hardware_tested':False},indent=2))
        finally:
            for process in processes:
                if process.poll() is None:process.kill();process.wait(timeout=5)
                process.error_file.close()
            for pid in owned_workers:
                try:
                    if str(worker).encode() in Path(f'/proc/{pid}/cmdline').read_bytes():os.kill(pid,signal.SIGKILL)
                except (ProcessLookupError,FileNotFoundError):pass

if __name__=='__main__':main()
