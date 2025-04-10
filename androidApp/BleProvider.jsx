import React, { createContext, useState, useEffect } from 'react';
import { BleManager } from 'react-native-ble-plx';
import { PermissionsAndroid, Platform, AppState } from 'react-native';
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
    usernameServiceUUID: "4fafc200-1fb5-459e-8fcc-c5c9c331914b",
    usernameCharacteristicUUID: "beb5483e-36e1-4688-b7f5-ea07361b26a8",
  };

  // Manejo del AppState para diferenciar entre segundo plano y cierre total
  useEffect(() => {
    requestPermissions();
    attemptAutoReconnect();

    const appStateSubscription = AppState.addEventListener('change', nextAppState => {
      // Podemos registrar cambios, pero no desconectamos si la app va a segundo plano,
      // ya que se requiere mantener la conexión en background.
      // Solo cuando el proveedor se desmonte (cierre de app) se desconectará.
      console.log("AppState changed to:", nextAppState);
    });

    return () => {
      // Al desmontarse el provider (cierre de la app), se llamará a disconnect()
      disconnect();
      appStateSubscription.remove();
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
        console.log("Intentando reconexión automática con ID:", savedDeviceId);
        // Realiza un escaneo corto para confirmar la presencia del dispositivo
        let deviceFound = null;
        await bleManager.startDeviceScan(null, null, async (error, device) => {
          if (error) {
            console.warn("Error en escaneo:", error.message);
            bleManager.stopDeviceScan();
            return;
          }
          if (device && device.id === savedDeviceId) {
            deviceFound = device;
            bleManager.stopDeviceScan();
          }
        });
        // Esperar unos segundos (por ejemplo, 5 segundos) para el escaneo
        await new Promise(resolve => setTimeout(resolve, 5000));

        if (deviceFound) {
          const connected = await bleManager.connectToDevice(savedDeviceId);
          await connected.discoverAllServicesAndCharacteristics();
          setConnectedDevice(connected);
          console.log("Reconexión automática exitosa:", connected.name || connected.localName);
        } else {
          console.warn("Dispositivo con ID guardado no encontrado durante el escaneo.");
        }
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
        console.log("Desconexión exitosa");
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
