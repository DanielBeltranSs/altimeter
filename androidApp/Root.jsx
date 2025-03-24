import React from 'react';
import App from './App';
import { BleProvider } from './BleProvider';

const Root = () => (
  <BleProvider>
    <App />
  </BleProvider>
);

export default Root;
