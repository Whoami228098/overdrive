# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render the actual store on Linux with owned audio/catalog fixtures, without a console."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import socket
import subprocess
import time

from PIL import Image
from cli import desktop, preview_env, sandbox
from common import ROOT


def main():
    out=ROOT.parent/'quality-evidence/store-preview'
    out.mkdir(parents=True,exist_ok=True)
    stage=ROOT/'.build/store-ui-app'
    shutil.copytree(ROOT.parent/'app',stage,dirs_exist_ok=True)
    media=stage/'media';media.mkdir(exist_ok=True)
    tracks=[]
    for index,frequency in enumerate([440,660]):
        filename=f'fixture-{index}.mp3'
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i',f'sine=frequency={frequency}:duration=7:sample_rate=44100',
                        '-ac','2','-c:a','libmp3lame','-y',str(media/filename)],check=True)
        tracks.append({'id':f'fixture-{index}','title':f'Owned Audio Fixture {index}',
                       'artist':'Native audio test','file':filename,'cover':'cover.png','duration':7})
    Image.open(stage/'assets/ice-mark.png').convert('RGB').resize((256,256)).save(media/'cover.png')
    (media/'playlist.json').write_text(json.dumps({'version':1,'tracks':tracks}))
    # A data-only fixture exercises the same file checks; it is never applied to game memory.
    payload=b'{"name":"Owned Fixture","id":"CUSA99999","version":"01.00","mods":[]}'
    digest=hashlib.sha256(payload).hexdigest()
    record={'id':'f'*24,'host':'ps5','platform':'ps4','titleId':'CUSA99999','title':'Owned store fixture',
            'version':'01.00','format':'json','loader':'cheats','features':['Owned test option'],
            'authors':['Test fixture'],'categories':['gameplay'],'sha256':digest,'bytes':len(payload),'file':f'payload/{digest}.json'}
    mods=stage/'mods';(mods/'payload').mkdir(exist_ok=True)
    (mods/record['file']).write_bytes(payload)
    (mods/'hosts/ps5/titles').mkdir(exist_ok=True)
    (mods/'hosts/ps5/titles/CUSA99999.json').write_text(json.dumps({'version':1,'records':[record]}))
    (mods/'hosts/ps5/index.json').write_text(json.dumps({'version':1,'host':'ps5','titles':[
        {'titleId':'CUSA99999','title':'Owned store fixture','platform':'ps4','versions':['01.00'],
         'count':1,'file':'hosts/ps5/titles/CUSA99999.json'}]}))
    command,build=desktop(stage)
    fixture=sandbox(stage)
    for directory in ['data/homebrew/covers','data/cheatrunner','user/appmeta/CUSA99999']:
        (fixture/directory).mkdir(parents=True,exist_ok=True)
    (fixture/'data/homebrew/catalog-user.json').write_text(json.dumps({'games':[
        {'title':'Owned store fixture','platform':'ps4','title_id':'CUSA99999','version':'01.00',
         'source':'https://example.invalid/owned-fixture.pkg','size_gb':0.001,
         'cover_url':'file:///covers/fixture.png'}]}))
    shutil.copyfile(media/'cover.png',fixture/'data/homebrew/covers/fixture.png')
    (fixture/'user/appmeta/CUSA99999/param.json').write_text(json.dumps({'titleId':'CUSA99999','contentVersion':'01.00'}))
    control=str(out/'control.sock')
    env={**preview_env(stage),'PS5_REACT_CONTROL':control,'SDL_AUDIODRIVER':'dummy'}
    actions=[]
    with (out/'native.log').open('w') as log:
        process=subprocess.Popen(list(map(str,command)),cwd=build,env=env,stdout=log,stderr=subprocess.STDOUT)
        def send(line):
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as client:
                client.settimeout(10);client.connect(control);client.sendall((line+'\n').encode())
                data=b''
                while chunk:=client.recv(65536):data+=chunk
            return data.decode().strip()
        def records():return [x for x in json.loads(send('snapshot')) if x.get('visible') and not x.get('inert')]
        def wait(seconds):
            deadline=time.monotonic()+seconds
            while time.monotonic()<deadline:
                if process.poll() is not None:raise RuntimeError(f'Preview exited {process.returncode}')
                time.sleep(0.1)
        def shot(name):
            target=out/f'{name}.bmp'
            reply=send(f'shot {target} full')
            if reply!=str(target):raise RuntimeError(reply)
            image=Image.open(target);image.save(out/f'{name}.png');target.unlink()
        def press(key):
            result=send('focus '+key)
            if result!='ok':raise RuntimeError(result)
            if send('press confirm')!='ok':raise RuntimeError('Confirm was refused')
            actions.append(key);wait(0.7)
        try:
            deadline=time.monotonic()+30
            while True:
                try:send('wait 0');break
                except (OSError,TimeoutError):
                    if process.poll() is not None or time.monotonic()>deadline:raise
                    time.sleep(0.05)
            wait(0.8);shot('01-ice-assembly')
            wait(4);shot('02-store')
            # This fixture has no hero art, so Spotlight is empty. The visible grid must
            # still acquire its cover before focus enters it, rather than keep its red tint.
            with Image.open(out/'02-store.png') as frame:
                pixels=list(frame.convert('RGB').crop((60,580,300,900)).getdata())
            placeholder=sum(r>150 and g<70 and b<80 for r,g,b in pixels)/len(pixels)
            if placeholder>.05:raise RuntimeError(f'Visible grid cover stayed a red placeholder: {placeholder:.1%}')
            music=next((x for x in records() if x.get('focusKey')=='nav:music'),None)
            if not music or 'Owned Audio Fixture' not in music.get('text',''):raise RuntimeError('Autoplay widget missing')
            press('nav:music');shot('03-paused')
            if not any('Paused' in x.get('text','') for x in records()):raise RuntimeError('Pause UI not shown')
            press('nav:music')
            # Use the same stick-button state path as a real DualSense, not a focus action.
            if send('hold r3 150')!='ok':raise RuntimeError('R3 input refused')
            wait(.5)
            if not any('Paused' in x.get('text','') for x in records()):raise RuntimeError('R3 did not pause music')
            if send('hold r3 150')!='ok':raise RuntimeError('R3 resume input refused')
            wait(.5)
            if any('Paused' in x.get('text','') for x in records()):raise RuntimeError('R3 did not resume music')
            actions.extend(['controller:r3:pause','controller:r3:resume'])
            # The index page is present even when the store's built-in catalog is empty.
            press('card:CUSA99999');shot('04-game')
            press('mods:CUSA99999');shot('05-mods')
            press('mods-pack:'+record['id']);shot('06-mod-details')
            press('mods-install')
            destination=fixture/'data/cheatrunner/cheats/json/CUSA99999_01.00.json'
            deadline=time.monotonic()+10
            while not destination.exists() and time.monotonic()<deadline:wait(0.1)
            if not destination.exists() or destination.read_bytes()!=payload:raise RuntimeError('Verified mod data was not installed')
            press('mods-restore')
            deadline=time.monotonic()+10
            while destination.exists() and time.monotonic()<deadline:wait(0.1)
            if destination.exists():raise RuntimeError('Restore did not remove the owned mod data')
            wait(8)
            (out/'focus.json').write_text(json.dumps(records(),indent=2))
            send('quit');process.wait(timeout=15)
        finally:
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:process.kill();process.wait()
    output=(out/'native.log').read_text()
    if process.returncode or 'JS exception' in output or 'AddressSanitizer' in output:raise RuntimeError('Native preview failed; inspect native.log')
    if 'Image.load ' in output or 'could not read local image' in output:raise RuntimeError('A local cover failed to load; inspect the screenshots and native.log')
    if output.count('music decoded id=')<2 or 'music ended id=' not in output:raise RuntimeError('Real MP3 stream did not finish and continue')
    result={'passed':True,'hardware_tested':False,'host':'Linux SDL/OpenGL/QuickJS',
            'visible_grid_cover_loaded_with_empty_spotlight':True,
            'source':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
            'audio':'Owned sine MP3s through the actual worker/mixer and dummy output',
            'actions':actions,'screenshots':[p.name for p in sorted(out.glob('*.png'))]}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)


if __name__=='__main__':main()
