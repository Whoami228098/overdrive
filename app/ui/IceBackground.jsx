import {memo} from 'react';
import {Image, Platform, View} from '@ps5-react/core';
import background from '../assets/ice-background.png';

// A static shared layer never schedules background animation during browsing.
export const IceBackground = memo(function IceBackground() {
  return <View style={{position: 'absolute', left: 0, top: 0, right: 0, bottom: 0}}>
    <Image source={background} layer={Platform.OS !== 'ps4'} resizeMode="cover"
      style={{position: 'absolute', left: 0, top: 0, width: 1920, height: 1080}} />
    <View style={{position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: '#00000099'}} />
  </View>;
});
