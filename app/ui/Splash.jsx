import {useEffect, useRef, useState} from 'react';
import {Image, Text, View, motion} from '@ps5-react/core';
import ice from '../assets/ice-mark.png';

const SIZE = 420, COLUMNS = 3, ROWS = 4;
const PIECES = Array.from({length: COLUMNS * ROWS}, (_, index) => {
  const column = index % COLUMNS, row = Math.floor(index / COLUMNS);
  const angle = index * Math.PI * 2 / (COLUMNS * ROWS) - Math.PI / 2;
  return {index, column, row, x: Math.cos(angle) * 1050, y: Math.sin(angle) * 760,
    rotate: index % 2 ? 22 : -22, delay: (index % 4) * 0.045};
});

export function Splash({onDone}) {
  const [leaving, setLeaving] = useState(false);
  const completed = useRef(false), done = useRef(onDone);
  done.current = onDone;
  useEffect(() => {
    const assembled = setTimeout(() => setLeaving(true), 2000);
    return () => clearTimeout(assembled);
  }, []);
  const finish = () => {
    if (!leaving || completed.current) return;
    completed.current = true;
    done.current?.();
  };
  return <motion.View style={{position: 'absolute', left: 0, top: 0, right: 0, bottom: 0,
      backgroundColor: '#000000', alignItems: 'center', justifyContent: 'center'}}
    initial={false} animate={{opacity: leaving ? 0 : 1}} transition={{duration: 0.3}}
    onAnimationComplete={finish}>
    <View style={{width: SIZE, height: SIZE}}>
      {PIECES.map(piece => <motion.View key={piece.index}
        style={{position: 'absolute', left: piece.column * SIZE / COLUMNS, top: piece.row * SIZE / ROWS,
          width: SIZE / COLUMNS, height: SIZE / ROWS, overflow: 'hidden'}}
        initial={{x: piece.x, y: piece.y, rotate: piece.rotate, opacity: 0}}
        animate={{x: 0, y: 0, rotate: 0, opacity: 1}}
        transition={{duration: 1.3, delay: piece.delay, ease: 'easeOut'}}>
        <Image source={ice} resizeMode="contain" style={{position: 'absolute', width: SIZE, height: SIZE,
          left: -piece.column * SIZE / COLUMNS, top: -piece.row * SIZE / ROWS}} />
      </motion.View>)}
    </View>
    <motion.View initial={{opacity: 0, y: 12}} animate={{opacity: 1, y: 0}}
      transition={{duration: 0.4, delay: 1.2}} style={{marginTop: 28}}>
      <Text style={{fontFamily: 'Inter-Regular', fontSize: 23, letterSpacing: 7, color: '#e9f4fa',
        fontWeight: 600}}>TRAPSTARSHOP</Text>
    </motion.View>
  </motion.View>;
}
