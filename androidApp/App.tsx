import React, { useState, useEffect } from 'react';
import { 
  View, 
  Text, 
  Button, 
  FlatList, 
  TouchableOpacity, 
  StyleSheet, 
  ActivityIndicator, 
  Alert,
  PermissionsAndroid,
  Platform
} from 'react-native';
import { BleManager } from 'react-native-ble-plx';
import UpdateUsername from './UpdateUsername';
import FirmwareUpdate from './FirmwareUpdate';

export default function App() {
  const [bleManager] = useState(new BleManager());
  const [scanning, setScanning] = useState(false);
  const [devices, setDevices] = useState([]);
  const [connectedDevice, setConnectedDevice] = useState(null);
  const [connecting, setConnecting] = useState(false);
  const [screen, setScreen] = useState('main'); // "main", "updateUser", "firmwareUpdate"

  // Solicitar permisos en Android
  const requestPermissions = async () => {
    if (Platform.OS === 'android') {
      try {
        const granted = await PermissionsAndroid.requestMultiple([
          PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
        ]);
        console.log('Permisos BLE:', granted);
      } catch (error) {
        console.warn('Error solicitando permisos BLE:', error);
      }
    }
  };

  useEffect(() => {
    requestPermissions();
    return () => bleManager.destroy();
  }, []);

  // Escanear dispositivos BLE
  const scanForDevices = () => {
    setDevices([]);
    setScanning(true);
    console.log('Iniciando escaneo BLE...');
    bleManager.startDeviceScan(null, null, (error, device) => {
      if (error) {
        console.error('Error escaneando:', error);
        setScanning(false);
        return;
      }
      const name = device?.name || device?.localName;
      console.log('Dispositivo detectado:', name, device?.id);
      if (device && name) {
        setDevices(prev => prev.some(d => d.id === device.id) ? prev : [...prev, device]);
      }
    });
    setTimeout(() => {
      bleManager.stopDeviceScan();
      setScanning(false);
      console.log('Escaneo detenido (timeout)');
    }, 10000);
  };

  // Conectar a dispositivo BLE
  const connectToDevice = async (device) => {
    try {
      setConnecting(true);
      bleManager.stopDeviceScan();
      setScanning(false);
      console.log('Conectando a:', device.name || device.localName);
      const connected = await device.connect();
      await connected.discoverAllServicesAndCharacteristics();
      setConnectedDevice(connected);
      Alert.alert('Conectado', `Conectado a ${connected.name || connected.localName}`);
    } catch (error) {
      console.error('Error al conectar:', error);
      Alert.alert('Error', 'No se pudo conectar al dispositivo');
    } finally {
      setConnecting(false);
    }
  };

  const disconnectDevice = async () => {
    if (connectedDevice) {
      await connectedDevice.cancelConnection();
      setConnectedDevice(null);
    }
  };

  // Pantalla principal
  const renderMainScreen = () => (
    <View style={styles.container}>
      <Text style={styles.status}>
        {connectedDevice ? `Conectado: ${connectedDevice.name || connectedDevice.localName}` : 'Desconectado'}
      </Text>
      {!connectedDevice ? (
        <>
          <Button title="Escanear Dispositivos" onPress={scanForDevices} disabled={scanning || connecting} />
          {(scanning || connecting) && <ActivityIndicator size="large" color="#007AFF" style={styles.loading} />}
          <FlatList
            data={devices}
            keyExtractor={item => item.id}
            renderItem={({ item }) => (
              <TouchableOpacity style={styles.deviceItem} onPress={() => connectToDevice(item)} disabled={connecting}>
                <Text>{item.name || item.localName}</Text>
                <Text style={styles.deviceId}>{item.id}</Text>
              </TouchableOpacity>
            )}
            ListEmptyComponent={() => <Text style={styles.noDevices}>No se encontraron dispositivos</Text>}
          />
        </>
      ) : (
        <View style={styles.buttonsContainer}>
          <Button title="Actualizar Usuario" onPress={() => setScreen('updateUser')} />
          <Button title="Actualizar Firmware" onPress={() => setScreen('firmwareUpdate')} />
          <Button title="Desconectar" onPress={disconnectDevice} />
        </View>
      )}
    </View>
  );

  const renderScreen = () => {
    if (screen === 'updateUser') return <UpdateUsername onBack={() => setScreen('main')} connectedDevice={connectedDevice} />;
    if (screen === 'firmwareUpdate') return <FirmwareUpdate onBack={() => setScreen('main')} connectedDevice={connectedDevice} />;
    return renderMainScreen();
  };

  return <View style={styles.appContainer}>{renderScreen()}</View>;
}

const styles = StyleSheet.create({
  appContainer: { flex: 1, padding: 20, backgroundColor: '#fff' },
  container: { flex: 1 },
  status: { fontSize: 18, textAlign: 'center', marginBottom: 10 },
  loading: { marginVertical: 10 },
  deviceItem: { padding: 10, borderBottomWidth: 1, borderColor: '#ccc' },
  deviceId: { fontSize: 12, color: '#555' },
  noDevices: { textAlign: 'center', marginTop: 20, color: '#999' },
  buttonsContainer: { marginTop: 20, justifyContent: 'space-around', height: 150 },
});
