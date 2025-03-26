// BleProvider.jsx
import React, { createContext, useState, useEffect } from 'react';
import { BleManager } from 'react-native-ble-plx';
import { Alert, PermissionsAndroid, Platform } from 'react-native';

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

  const requestPermissions = async () => {
    if (Platform.OS === 'android') {
      console.log("Solicitando permisos BLE...");
      try {
        const granted = await PermissionsAndroid.requestMultiple([
          PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
        ]);
        console.log('Permisos solicitados:', granted);
      } catch (err) {
        console.warn('Error al solicitar permisos BLE:', err);
      }
    }
  };

  useEffect(() => {
    requestPermissions();
    return () => {
      bleManager.destroy();
    };
  }, [bleManager]);

  const scanAndConnect = async (targetDeviceName) => {
    return new Promise((resolve, reject) => {
      let found = false;
      console.log("Iniciando escaneo...");
      bleManager.startDeviceScan(null, null, async (error, device) => {
        if (error) {
          console.error("Error durante el escaneo:", error);
          bleManager.stopDeviceScan();
          reject(error);
          return;
        }
        const deviceName = device ? (device.name || device.localName) : null;
        console.log("Dispositivo detectado:", deviceName, device?.id);
        if (!found && device && deviceName && deviceName.includes(targetDeviceName)) {
          found = true;
          console.log("Dispositivo encontrado:", deviceName, device.id);
          bleManager.stopDeviceScan();
          try {
            console.log("Intentando conectar a:", deviceName, device.id);
            const connected = await device.connect();
            await new Promise(resolve => setTimeout(resolve, 500));
            await connected.discoverAllServicesAndCharacteristics();
            setConnectedDevice(connected);
            console.log("Dispositivo conectado:", connected.name || connected.localName, connected.id);
            const services = await connected.services();
            console.log("Servicios descubiertos:", services.length);
            resolve(connected);
          } catch (err) {
            console.error("Error al conectar o descubrir servicios:", err);
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
