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
  const [deviceServices, setDeviceServices] = useState([]); // Estado para servicios y características
  const [screen, setScreen] = useState('main'); // "main", "updateUser" o "firmwareUpdate"

  // Solicitar permisos en tiempo de ejecución para Android
  const requestPermissions = async () => {
    if (Platform.OS === 'android') {
      try {
        const granted = await PermissionsAndroid.requestMultiple([
          PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
        ]);
        console.log('Permisos solicitados:', granted);
      } catch (error) {
        console.warn('Error solicitando permisos:', error);
      }
    }
  };

  useEffect(() => {
    requestPermissions();
    return () => {
      bleManager.destroy();
    };
  }, [bleManager]);

  // Cuando se conecte un dispositivo, consulta sus servicios y características
  useEffect(() => {
    const fetchServices = async () => {
      if (connectedDevice) {
        try {
          const services = await connectedDevice.services();
          const servicesData = [];
          for (const service of services) {
            const characteristics = await service.characteristics();
            servicesData.push({
              serviceUUID: service.uuid,
              characteristics: characteristics.map(char => char.uuid),
            });
          }
          setDeviceServices(servicesData);
          console.log("Servicios obtenidos:", servicesData);
        } catch (err) {
          console.error("Error obteniendo servicios:", err);
        }
      } else {
        setDeviceServices([]);
      }
    };
    fetchServices();
  }, [connectedDevice]);

  // Función para escanear dispositivos BLE
  const scanForDevices = () => {
    setDevices([]); // Limpiar lista anterior
    setScanning(true);
    console.log("Iniciando escaneo...");
    bleManager.startDeviceScan(null, null, (error, device) => {
      if (error) {
        console.log("Error escaneando:", error);
        setScanning(false);
        return;
      }
      // Mostrar log de cada dispositivo detectado
      console.log("Dispositivo detectado:", device?.name || device?.localName, device?.id);
      if (device && (device.name || device.localName)) {
        setDevices(prevDevices => {
          const exists = prevDevices.some(d => d.id === device.id);
          if (!exists) {
            return [...prevDevices, device];
          }
          return prevDevices;
        });
      }
    });
    setTimeout(() => {
      bleManager.stopDeviceScan();
      setScanning(false);
      console.log("Escaneo detenido");
    }, 10000);
  };

  // Función para conectar al dispositivo seleccionado
  const connectToDevice = async (device) => {
    try {
      setConnecting(true);
      console.log("Intentando conectar a:", device.name || device.localName, device.id);
      const connected = await device.connect();
      await connected.discoverAllServicesAndCharacteristics();
      setConnectedDevice(connected);
      setConnecting(false);
      Alert.alert("Conectado", `Conectado a ${connected.name || connected.localName}`);
    } catch (error) {
      setConnecting(false);
      console.log("Error al conectar:", error);
      Alert.alert("Error", "No se pudo conectar al dispositivo");
    }
  };

  // Función para desconectar
  const disconnectDevice = async () => {
    if (connectedDevice) {
      try {
        await connectedDevice.cancelConnection();
        setConnectedDevice(null);
      } catch (error) {
        console.log("Error al desconectar:", error);
      }
    }
  };

  // Renderizado de los servicios y características
  const renderServices = () => {
    return (
      <View style={styles.servicesContainer}>
        <Text style={styles.servicesTitle}>Servicios y Características</Text>
        {deviceServices.length === 0 ? (
          <Text style={styles.noServices}>No se encontraron servicios.</Text>
        ) : (
          deviceServices.map((serviceData, index) => (
            <View key={index} style={styles.serviceItem}>
              <Text style={styles.serviceUUID}>Service: {serviceData.serviceUUID}</Text>
              {serviceData.characteristics.map((charUUID, idx) => (
                <Text key={idx} style={styles.charUUID}>Characteristic: {charUUID}</Text>
              ))}
            </View>
          ))
        )}
      </View>
    );
  };

  // Pantalla principal: muestra estado de conexión, lista de dispositivos y botones de navegación
  const renderMainScreen = () => (
    <View style={styles.container}>
      <Text style={styles.status}>
        {connectedDevice ? `Conectado: ${connectedDevice.name || connectedDevice.localName}` : "Desconectado"}
      </Text>
      {!connectedDevice && (
        <View style={styles.scanContainer}>
          <Button 
            title="Escanear Dispositivos" 
            onPress={scanForDevices} 
            disabled={scanning || connecting} 
          />
          {(scanning || connecting) && (
            <ActivityIndicator 
              size="large" 
              color="#007AFF" 
              style={styles.loading} 
            />
          )}
          <FlatList
            data={devices}
            keyExtractor={(item) => item.id}
            renderItem={({ item }) => (
              <TouchableOpacity 
                style={styles.deviceItem} 
                onPress={() => connectToDevice(item)}
                disabled={connecting}
              >
                <Text>{item.name || item.localName}</Text>
                <Text style={styles.deviceId}>{item.id}</Text>
              </TouchableOpacity>
            )}
            ListEmptyComponent={() => (
              <Text style={styles.noDevices}>No se encontraron dispositivos</Text>
            )}
          />
        </View>
      )}
      {connectedDevice && (
        <View style={styles.buttonsContainer}>
          <Button 
            title="Actualizar Usuario" 
            onPress={() => setScreen('updateUser')}
            disabled={!connectedDevice || connecting}
          />
          <Button 
            title="Actualizar Firmware" 
            onPress={() => setScreen('firmwareUpdate')}
            disabled={!connectedDevice || connecting}
          />
          <Button 
            title="Desconectar" 
            onPress={disconnectDevice}
            disabled={connecting}
          />
        </View>
      )}
      {/* Mostrar información de servicios si hay dispositivo conectado */}
      {connectedDevice && renderServices()}
    </View>
  );

  // Renderizado según la pantalla seleccionada
  const renderScreen = () => {
    if (screen === 'main') {
      return renderMainScreen();
    } else if (screen === 'updateUser') {
      return <UpdateUsername onBack={() => setScreen('main')} connectedDevice={connectedDevice} />;
    } else if (screen === 'firmwareUpdate') {
      return <FirmwareUpdate onBack={() => setScreen('main')} connectedDevice={connectedDevice} />;
    }
  };

  return (
    <View style={styles.appContainer}>
      {renderScreen()}
    </View>
  );
}

const styles = StyleSheet.create({
  appContainer: {
    flex: 1,
    padding: 20,
    backgroundColor: '#fff',
  },
  container: {
    flex: 1,
  },
  status: {
    fontSize: 18,
    textAlign: 'center',
    marginBottom: 10,
  },
  scanContainer: {
    flex: 1,
  },
  loading: {
    marginVertical: 10,
  },
  deviceItem: {
    padding: 10,
    borderBottomWidth: 1,
    borderColor: '#ccc',
  },
  deviceId: {
    fontSize: 12,
    color: '#555',
  },
  noDevices: {
    textAlign: 'center',
    marginTop: 20,
    color: '#999',
  },
  buttonsContainer: {
    marginTop: 20,
    justifyContent: 'space-around',
    height: 150,
  },
  servicesContainer: {
    marginTop: 20,
    borderTopWidth: 1,
    borderColor: '#ccc',
    paddingTop: 10,
  },
  servicesTitle: {
    fontSize: 16,
    fontWeight: 'bold',
    textAlign: 'center',
  },
  serviceItem: {
    marginVertical: 5,
  },
  serviceUUID: {
    fontSize: 14,
    fontWeight: 'bold',
  },
  charUUID: {
    fontSize: 12,
    marginLeft: 10,
  },
});
