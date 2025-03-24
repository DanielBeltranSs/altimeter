import React, { useState, useContext, useEffect } from 'react';
import { View, Text, TextInput, Button, Alert, StyleSheet, ActivityIndicator } from 'react-native';
import { BleContext } from './BleProvider';
import { Buffer } from 'buffer';

const TARGET_DEVICE_NAME = "ESP32-Altimetro-ota";

export default function UpdateUsername({ onBack }) {
  const { scanAndConnect, connectedDevice, config } = useContext(BleContext);
  const [username, setUsername] = useState('');
  const [updating, setUpdating] = useState(false);
  const [deviceServices, setDeviceServices] = useState([]);

  // Log para verificar el dispositivo conectado
  console.log("UpdateUsername: connectedDevice =", connectedDevice);

  useEffect(() => {
    const fetchServices = async () => {
      if (connectedDevice) {
        try {
          // Opcional: Forzar descubrimiento de servicios
          await connectedDevice.discoverAllServicesAndCharacteristics();
          const services = await connectedDevice.services();
          console.log("UpdateUsername: Número de servicios descubiertos =", services.length);
          const servicesData = [];
          for (const service of services) {
            const characteristics = await service.characteristics();
            servicesData.push({
              serviceUUID: service.uuid,
              characteristics: characteristics.map(char => char.uuid),
            });
          }
          setDeviceServices(servicesData);
          console.log("Servicios en UpdateUsername:", servicesData);
        } catch (err) {
          console.error("Error obteniendo servicios en UpdateUsername:", err);
        }
      } else {
        setDeviceServices([]);
      }
    };
    fetchServices();
  }, [connectedDevice]);

  const updateUsername = async () => {
    if (!username) {
      Alert.alert("Error", "Por favor ingresa un nombre de usuario");
      return;
    }
    setUpdating(true);
    try {
      let device;
      if (connectedDevice) {
        device = connectedDevice;
        console.log("Usando dispositivo conectado:", device.name || device.localName);
      } else {
        console.log("No hay dispositivo conectado, iniciando escaneo...");
        device = await scanAndConnect(TARGET_DEVICE_NAME);
      }
      if (!device) {
        throw new Error("No se pudo obtener un dispositivo conectado.");
      }
      const base64Username = Buffer.from(username).toString('base64');
      console.log("Enviando nombre (base64):", base64Username);
      await device.writeCharacteristicWithResponseForService(
        config.usernameServiceUUID,
        config.usernameCharacteristicUUID,
        base64Username
      );
      Alert.alert("Éxito", "Nombre de usuario actualizado correctamente");
    } catch (error) {
      console.error("Error en updateUsername:", error);
      Alert.alert("Error al actualizar el nombre de usuario", error.message);
    } finally {
      setUpdating(false);
    }
  };

  const renderServices = () => {
    if (!connectedDevice) return null;
    return (
      <View style={styles.servicesContainer}>
        <Text style={styles.servicesTitle}>Servicios y Características</Text>
        {deviceServices.length === 0 ? (
          <Text style={styles.noServices}>No se encontraron servicios.</Text>
        ) : (
          deviceServices.map((serviceData, index) => (
            <View key={index} style={styles.serviceItem}>
              <Text style={styles.serviceUUID}>
                Service: {serviceData.serviceUUID}
              </Text>
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
    flex: 1
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
