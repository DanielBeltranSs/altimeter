import React, { useEffect, useState } from 'react';
import { View, Text, Button, FlatList, PermissionsAndroid, Platform, TouchableOpacity } from 'react-native';
import { BleManager } from 'react-native-ble-plx';
import { Buffer } from 'buffer';

export default function App() {
  const [bleManager] = useState(new BleManager());
  const [altitude, setAltitude] = useState('---');
  const [scanning, setScanning] = useState(false);
  const [devices, setDevices] = useState([]);

  useEffect(() => {
    requestPermissions();
    const subscription = bleManager.onStateChange((state) => {
      console.log('Estado BLE:', state);
      if (state === 'PoweredOn') {
        console.log('BLE listo para escanear');
      }
    }, true);
    return () => subscription.remove();
  }, []);

  const requestPermissions = async () => {
    if (Platform.OS === 'android') {
      await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN);
      await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT);
      await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION, {
        title: 'Permiso de Ubicación',
        message: 'La app necesita acceso a la ubicación para escanear dispositivos BLE.',
        buttonPositive: 'OK',
      });
    }
  };

  const startScan = () => {
    if (scanning) return;
    setDevices([]); // Limpiar la lista antes de escanear
    setScanning(true);
    bleManager.startDeviceScan(null, null, (error, device) => {
      if (error) {
        console.warn('Error escaneando:', error);
        setScanning(false);
        return;
      }
      // Si se detecta un dispositivo, lo agregamos a la lista si no está ya incluido
      if (device && device.id) {
        setDevices(prevDevices => {
          if (!prevDevices.find(d => d.id === device.id)) {
            console.log('Dispositivo encontrado:', device.name || 'Sin nombre', device.id);
            return [...prevDevices, device];
          }
          return prevDevices;
        });
      }
    });
  };

  const stopScan = () => {
    bleManager.stopDeviceScan();
    setScanning(false);
  };

  const connectToDevice = (device) => {
    stopScan();
    device.connect()
      .then((d) => d.discoverAllServicesAndCharacteristics())
      .then((d) => {
        d.monitorCharacteristicForService(
          '4fafc201-1fb5-459e-8fcc-c5c9c331914b',
          'beb5483e-36e1-4688-b7f5-ea07361b26a8',
          (error, characteristic) => {
            if (error) {
              console.warn('Error en monitor:', error);
              return;
            }
            const base64Value = characteristic?.value;
            if (base64Value) {
              const ascii = Buffer.from(base64Value, 'base64').toString('ascii');
              console.log('Altitud recibida:', ascii);
              setAltitude(ascii);
            }
          }
        );
      })
      .catch(err => console.warn('Error conectando:', err));
  };

  return (
    <View style={{ marginTop: 50, padding: 20 }}>
      <Text style={{ fontSize: 18 }}>Altitud: {altitude}</Text>
      <Button title={scanning ? "Detener escaneo" : "Escanear BLE"} onPress={scanning ? stopScan : startScan} />
      <FlatList
        data={devices}
        keyExtractor={(item) => item.id}
        renderItem={({ item }) => (
          <TouchableOpacity onPress={() => connectToDevice(item)}>
            <Text style={{ padding: 10, fontSize: 16 }}>
              {item.name ? item.name : 'Sin nombre'} - {item.id}
            </Text>
          </TouchableOpacity>
        )}
        ListEmptyComponent={() => (
          <Text style={{ marginTop: 20 }}>No se encontraron dispositivos</Text>
        )}
      />
    </View>
  );
}
