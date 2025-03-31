// UpdateUsername.js
import React, { useState, useContext, useEffect } from 'react';
import { View, Text, TextInput, Button, Alert, StyleSheet, ActivityIndicator } from 'react-native';
import { BleContext } from './BleProvider';
import { Buffer } from 'buffer';

// BLE – UUIDs para actualización de usuario
const USERNAME_SERVICE_UUID = "12345678-1234-1234-1234-1234567890ab";
const USERNAME_CHARACTERISTIC_UUID = "abcd1234-ab12-cd34-ef56-1234567890ab";

const TARGET_DEVICE_NAME = "ESP32-FIR";

export default function UpdateUsername({ onBack }) {
  const { scanAndConnect, connectedDevice } = useContext(BleContext);
  const [username, setUsername] = useState('');
  const [updating, setUpdating] = useState(false);
  const [deviceServices, setDeviceServices] = useState([]);
  const [loadingServices, setLoadingServices] = useState(true);

  useEffect(() => {
    const fetchServices = async () => {
      if (connectedDevice) {
        try {
          console.log("🔍 Descubriendo servicios y características...");
          await connectedDevice.discoverAllServicesAndCharacteristics();
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
          console.log("🧩 Servicios descubiertos:", servicesData);
        } catch (err) {
          console.error("❌ Error obteniendo servicios:", err);
        }
      } else {
        setDeviceServices([]);
      }
      setLoadingServices(false);
    };

    if (connectedDevice) {
      fetchServices();
    }
  }, [connectedDevice]);

  const updateUsername = async () => {
    if (!username) {
      Alert.alert("Error", "Por favor ingresa un nombre de usuario");
      return;
    }
    setUpdating(true);
    try {
      let device = connectedDevice;
      if (!device) {
        console.log("No hay dispositivo conectado, iniciando escaneo...");
        device = await scanAndConnect(TARGET_DEVICE_NAME);
      }
      if (!device) {
        throw new Error("No se pudo obtener un dispositivo conectado.");
      }
      await device.discoverAllServicesAndCharacteristics();

      // Convertir el nombre de usuario a base64 antes de enviarlo
      const base64Username = Buffer.from(username).toString('base64');
      console.log("📤 Enviando nombre en base64:", base64Username);

      await device.writeCharacteristicWithResponseForService(
        USERNAME_SERVICE_UUID,
        USERNAME_CHARACTERISTIC_UUID,
        base64Username
      );
      Alert.alert("✅ Éxito", "Nombre de usuario actualizado correctamente");
    } catch (error) {
      console.error("Error en updateUsername:", error);
      Alert.alert("❌ Error", error.message);
    } finally {
      setUpdating(false);
    }
  };

  const renderServices = () => {
    if (loadingServices) {
      return (
        <View style={styles.servicesContainer}>
          <ActivityIndicator size="small" color="#007AFF" />
          <Text style={{ textAlign: 'center', marginTop: 5 }}>Cargando servicios...</Text>
        </View>
      );
    }
    if (!connectedDevice) return null;
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
                <Text key={idx} style={styles.charUUID}>
                  Characteristic: {charUUID}
                </Text>
              ))}
            </View>
          ))
        )}
      </View>
    );
  };

  return (
    <View style={styles.container}>
      <Text style={styles.title}>Actualizar Nombre de Usuario</Text>
      <TextInput
        style={styles.input}
        placeholder="Ingresa nuevo nombre de usuario"
        value={username}
        onChangeText={setUsername}
      />
      {updating ? (
        <ActivityIndicator size="large" color="#007AFF" />
      ) : (
        <Button title="Actualizar" onPress={updateUsername} />
      )}
      {onBack && (
        <View style={styles.backButton}>
          <Button title="Volver" onPress={onBack} />
        </View>
      )}
      {renderServices()}
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    padding: 20,
    backgroundColor: '#fff',
    flex: 1,
  },
  title: {
    fontSize: 20,
    marginBottom: 15,
    textAlign: 'center',
  },
  input: {
    height: 50,
    borderColor: '#ccc',
    borderWidth: 1,
    borderRadius: 5,
    paddingHorizontal: 10,
    marginBottom: 20,
  },
  backButton: {
    marginTop: 10,
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
    marginBottom: 5,
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
  noServices: {
    textAlign: 'center',
    color: '#999',
  },
});
