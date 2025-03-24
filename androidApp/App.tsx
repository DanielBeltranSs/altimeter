// App.js
import React, { useEffect, useState } from 'react';
import { View, Text, Button, Platform, PermissionsAndroid } from 'react-native';
import { BleManager } from 'react-native-ble-plx';
// Para decodificar el valor base64
import { Buffer } from 'buffer';

export default function App() {
  const [bleManager] = useState(new BleManager());
  const [altitude, setAltitude] = useState('---');

  useEffect(() => {
    // Al montar el componente, pedimos permisos en Android 12+
    requestPermissions();
  }, []);

  const requestPermissions = async () => {
    if (Platform.OS === 'android') {
      // Android 12+ pide BLUETOOTH_SCAN y BLUETOOTH_CONNECT
      const scanGranted = await PermissionsAndroid.request(
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
        {
          title: 'Permiso para escanear Bluetooth',
          message: 'Necesitamos acceder al escaneo BLE para encontrar tu altímetro.',
          buttonPositive: 'OK',
        }
      );
      const connectGranted = await PermissionsAndroid.request(
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
        {
          title: 'Permiso para conectar Bluetooth',
          message: 'Necesitamos conectar a tu altímetro para leer los datos.',
          buttonPositive: 'OK',
        }
      );
      // Podrías chequear si fueron concedidos o no:
      if (scanGranted !== PermissionsAndroid.RESULTS.GRANTED ||
          connectGranted !== PermissionsAndroid.RESULTS.GRANTED) {
        console.log('Permisos BLE denegados');
      }
    }
  };

  const startScan = () => {
    // Iniciamos el escaneo de dispositivos
    bleManager.startDeviceScan(null, null, (error, device) => {
      if (error) {
        console.warn('Error escaneando BLE:', error);
        return;
      }

      // Verificamos si es nuestro ESP32 por nombre (puedes filtrar por MAC, etc.)
      if (device?.name === 'ESP32-Altimetro') {
        console.log('Encontrado:', device.name);

        // Detenemos el escaneo
        bleManager.stopDeviceScan();

        // Nos conectamos al dispositivo
        device
          .connect()
          .then((connectedDevice) => {
            console.log('Conectado a:', connectedDevice.name);
            // Descubrir servicios y características
            return connectedDevice.discoverAllServicesAndCharacteristics();
          })
          .then((connectedDevice) => {
            // Ahora nos suscribimos a la característica que notifica la altitud
            connectedDevice.monitorCharacteristicForService(
              '4fafc201-1fb5-459e-8fcc-c5c9c331914b', // SERVICE_UUID
              'beb5483e-36e1-4688-b7f5-ea07361b26a8', // CHARACTERISTIC_UUID
              (error, characteristic) => {
                if (error) {
                  console.warn('Error al monitorizar característica:', error);
                  return;
                }
                // characteristic.value viene en base64
                const base64Value = characteristic?.value;
                if (base64Value) {
                  // Decodificar a string ASCII
                  const ascii = Buffer.from(base64Value, 'base64').toString('ascii');
                  console.log('Altitud recibida:', ascii);
                  setAltitude(ascii); // Guardamos en el estado para mostrar en pantalla
                }
              }
            );
          })
          .catch((err) => {
            console.warn('Error conectando o descubriendo servicios:', err);
          });
      }
    });
  };

  return (
    <View style={{ flex: 1, justifyContent: 'center', alignItems: 'center' }}>
      <Text style={{ fontSize: 20, marginBottom: 10 }}>Altímetro BLE</Text>
      <Button title="Escanear y Conectar" onPress={startScan} />
      <Text style={{ marginTop: 20 }}>
        Altitud: {altitude}
      </Text>
    </View>
  );
}
