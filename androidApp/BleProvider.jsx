import React, { createContext, useState, useEffect } from 'react';
import { BleManager } from 'react-native-ble-plx';
import { PermissionsAndroid, Platform } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';

export const BleContext = createContext({
  bleManager: null,
  connectedDevice: null,
  scanAndConnect: async (targetDeviceName) => {},
  disconnect: async () => {},
  config: { usernameServiceUUID: '', usernameCharacteristicUUID: '' },
});

export const BleProvider = ({ children }) => {
  const [bleManager] = useState(new BleManager());
  const [connectedDevice, setConnectedDevice] = useState(null);

  const config = {
    usernameServiceUUID: "4fafc201-1fb5-459e-8fcc-c5c9c331914b",
    usernameCharacteristicUUID: "beb5483e-36e1-4688-b7f5-ea07361b26a8",
  };

  useEffect(() => {
    requestPermissions();
    attemptAutoReconnect();
    return () => {
      bleManager.destroy();
    };
  }, []);

  const requestPermissions = async () => {
    if (Platform.OS === 'android') {
      try {
        await PermissionsAndroid.requestMultiple([
          PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
        ]);
      } catch (err) {
        console.warn('Error al solicitar permisos BLE:', err);
      }
    }
  };

  const attemptAutoReconnect = async () => {
    try {
      const savedDeviceId = await AsyncStorage.getItem('lastDeviceId');
      if (savedDeviceId) {
        console.log("Intentando reconexión automática con:", savedDeviceId);
        const device = await bleManager.connectToDevice(savedDeviceId);
        await device.discoverAllServicesAndCharacteristics();
        setConnectedDevice(device);
        console.log("Reconexión automática exitosa:", device.name || device.localName);
      }
    } catch (err) {
      console.warn("Fallo en reconexión automática:", err.message);
    }
  };

  const scanAndConnect = async (targetDeviceName) => {
    return new Promise((resolve, reject) => {
      let found = false;
      bleManager.startDeviceScan(null, null, async (error, device) => {
        if (error) {
          console.error("Error durante el escaneo:", error);
          bleManager.stopDeviceScan();
          reject(error);
          return;
        }

        const deviceName = device?.name || device?.localName || '';
        if (!found && device && deviceName.includes(targetDeviceName)) {
          found = true;
          bleManager.stopDeviceScan();
          try {
            const connected = await device.connect();
            await connected.discoverAllServicesAndCharacteristics();
            setConnectedDevice(connected);
            await AsyncStorage.setItem('lastDeviceId', connected.id);
            console.log("Dispositivo conectado y guardado:", connected.name || connected.localName);
            resolve(connected);
          } catch (err) {
            console.error("Error al conectar:", err);
            reject(err);
          }
        }
      });

      setTimeout(() => {
        if (!found) {
          bleManager.stopDeviceScan();
          reject(new Error("Dispositivo no encontrado en el tiempo esperado."));
        }
      }, 15000);
    });
  };

  const disconnect = async () => {
    if (connectedDevice) {
      try {
        await connectedDevice.cancelConnection();
        await AsyncStorage.removeItem('lastDeviceId');
        setConnectedDevice(null);
      } catch (error) {
        console.log("Error al desconectar:", error);
      }
    }
  };

  return (
    <BleContext.Provider value={{ bleManager, connectedDevice, scanAndConnect, disconnect, config }}>
      {children}
    </BleContext.Provider>
  );
};
