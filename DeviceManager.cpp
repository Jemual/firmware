/*
 * DeviceManager.cpp
 *
 * Created: 22/04/2013 22:02:14
 *  Author: mat
 */ 

#include "brewpi_avr.h"
#include "DeviceManager.h"
#include "TempControl.h"
#include "OneWireTempSensor.h"
#include "OneWireActuator.h"
#include "Actuator.h"
#include "Sensor.h"
#include "DisconnectedTempSensor.h"
#include "PiLink.h"
#include "EepromFormat.h"
#include "ds2413.h"
#include "OneWire.h"
#include "DallasTemperature.h"

DeviceManager deviceManager;

#if BREWPI_STATIC_CONFIG==BREWPI_SHIELD_REV_A
OneWire DeviceManager::beerSensorBus(beerSensorPin);
OneWire DeviceManager::fridgeSensorBus(fridgeSensorPin);
#elif BREWPI_STATIC_CONFIG==BREWPI_SHIELD_REV_C
OneWire DeviceManager::primaryOneWireBus(oneWirePin);
#endif

bool DeviceManager::firstDeviceOutput;



/*
 * Defaults for sensors, actuators and temperature sensors when not defined in the eeprom.
 */
ValueSensor<bool> defaultSensor(false);			// off
ValueActuator defaultActuator;
DisconnectedTempSensor defaultTempSensor;

OneWire* DeviceManager::oneWireBus(uint8_t pin) {
#if BREWPI_STATIC_CONFIG==BREWPI_SHIELD_REV_A
	if (pin==beerSensorPin)
		return &beerSensorBus;
	if (pin==fridgeSensorPin)
		return &fridgeSensorBus;
#elif BREWPI_STATIC_CONFIG==BREWPI_SHIELD_REV_C
	if (pin==oneWirePin)
		return &primaryOneWireBus;
#endif		
	return NULL;
}


/**
 * Sets devices to their unconfigured states. Each device is initialized to a static no-op instance.
 * This method is idempotent, and is called each time the eeprom is reset. 
 */
void DeviceManager::setupUnconfiguredDevices()
{	
	// right now, uninstall doesn't care about chamber/beer distinction.
	// but this will need to match beer/function when multiferment is available
	DeviceConfig cfg;	
	cfg.chamber = 1; cfg.beer = 1;		
	for (uint8_t i=0; i<DEVICE_MAX; i++) {
		cfg.deviceFunction = DeviceFunction(i);
		uninstallDevice(cfg);
	}
			
#if 0	// code from multichamber
	#if BREWPI_EMULATE	// use in-memory/emulated devices
	MockTempSensor directFridgeSensor(10,10);
	MockTempSensor directBeerSensor(5,5);
	#elif BREWPI_SIMULATE
	ExternalTempSensor directFridgeSensor(true);
	ExternalTempSensor directBeerSensor(true);
	#else  // non emulation - use real hardware devices
	OneWireTempSensor directFridgeSensor(fridgeSensorPin);
	OneWireTempSensor directBeerSensor(beerSensorPin);
	#endif

	#if BREWPI_EMULATE || BREWPI_SIMULATE
	ValueActuator heater;
	ValueActuator cooler;
	ValueSensor<bool> doorSensor((bool)false);
	#else
	DigitalPinActuator<heatingPin, SHIELD_INVERT> heater;
	DigitalPinActuator<coolingPin, SHIELD_INVERT> cooler;
	ValueSensor<bool> doorSensor((bool)false);
	//DigitalPinSensor<doorPin, SHIELD_INVERT, USE_INTERNAL_PULL_UP_RESISTORS> doorSensor;
	#endif

	#if LIGHT_AS_HEATER
	Actuator& light = heater;
	#else
	ValueActuator lightOn;	// eventually map the light to a real pin
	Actuator& light = lightOn;
	#endif

	TempSensor fridgeSensor(&directFridgeSensor);
	TempSensor beerSensor(&directBeerSensor);

	#if MULTICHAMBER
	MockTempSensor directFridgeSensor2(10,10);
	MockTempSensor directBeerSensor2(5,5);
	#if 1
	TempSensor fridgeSensor2(directFridgeSensor2);
	TempSensor beerSensor2(directBeerSensor2);
	#if 1 || BREWPI_EMULATE		// use emulator for now until we get multi-define one wire
	ValueActuator heat2;
	ValueActuator cool2;
	#endif
	#endif
	Chamber c1(fridgeSensor, beerSensor, cooler, heater, light, door);
	Chamber c2(fridgeSensor2, beerSensor2, cool2, heat2, light, door);

	Chamber* chambers[] = {
		&c1
		, &c2
	};

	ChamberManager chamberManager(chambers, sizeof(chambers)/sizeof(chambers[0]));
	#endif

	#if MULTICHAMBER
	chamberManager.init();
	for (chamber_id i=0; i<chamberManager.chamberCount(); i++) {
		chamberManager.initChamber(i);
		piLink.printFridgeAnnotation(PSTR("Arduino restarted. Chamber %d ready!"), i+1);
	}
	chamberManager.switchChamber(0);
	#else
	// setup state
	tempControl.beerSensor = &beerSensor;
	tempControl.fridgeSensor = &fridgeSensor;
	tempControl.cooler = &cooler;
	tempControl.heater = &heater;
	tempControl.door = &doorSensor;
	tempControl.light = &light;

	tempControl.loadSettingsAndConstants(); //read previous settings from EEPROM
	tempControl.init();
	tempControl.updatePID();
	tempControl.updateState();
		
	#endif
	#endif
}


/**
 * Creates a new device for the given config.
 */
void* DeviceManager::createDevice(DeviceConfig& config, DeviceType dt)
{
	switch (config.deviceHardware) {
		case DEVICE_HARDWARE_NONE:
			break;
		case DEVICE_HARDWARE_PIN:
			if (dt==DEVICETYPE_SWITCH_SENSOR)
				return new DigitalPinSensor(config.hw.pinNr, config.hw.invert);
			else
				return new DigitalPinActuator(config.hw.pinNr, config.hw.invert);
		case DEVICE_HARDWARE_ONEWIRE_TEMP:
			return new OneWireTempSensor(oneWireBus(config.hw.pinNr), config.hw.address, config.hw.calibration);
			
#if BREWPI_DS2413			
		case DEVICE_HARDWARE_ONEWIRE_2413:
			return new OneWireActuator(oneWireBus(config.hw.pinNr), config.hw.address, config.hw.pio, config.hw.invert);
#endif			
	}	
	return NULL;
}

/**
 * Returns the pointer to where the device pointer resides. This can be used to delete the current device and install a new one. 
 * For Temperature sensors, the returned pointer points to a TempSensor*. The basic device can be fetched by calling
 * TempSensor::getSensor().
 */
inline void** deviceTarget(DeviceConfig& config)
{
	// for multichamber, will write directly to the multi-chamber managed storage.
	// later...	
	if (config.chamber>1 || config.beer>1)
		return NULL;
	
	void** ppv;
	switch (config.deviceFunction) {
	case DEVICE_CHAMBER_ROOM_TEMP:
		ppv = (void**)&tempControl.ambientSensor;
		break;
	case DEVICE_CHAMBER_DOOR:
		ppv = (void**)&tempControl.door;
		break;
	case DEVICE_CHAMBER_LIGHT:
		ppv = (void**)&tempControl.light;
		break;
	case DEVICE_CHAMBER_HEAT:
		ppv = (void**)&tempControl.heater;
		break;
	case DEVICE_CHAMBER_COOL:
		ppv = (void**)&tempControl.cooler;
		break;
	case DEVICE_CHAMBER_TEMP:
		ppv = (void**)&tempControl.fridgeSensor;
		break;
	case DEVICE_BEER_TEMP:
		ppv = (void**)&tempControl.beerSensor;
		break;
	default:
		ppv = NULL;
	}
	return ppv;
}

// A pointer to a "temp sensor" may be a TempSensor* or a BasicTempSensor* .
// These functions allow uniform treatment.
inline bool isBasicSensor(DeviceFunction function) {
	// currently only ambient sensor is basic. The others are wrapped in a TempSensor.
	return function==DEVICE_CHAMBER_ROOM_TEMP;
}

inline BasicTempSensor& unwrapSensor(DeviceFunction f, void* pv) {
	return isBasicSensor(f) ? *(BasicTempSensor*)pv : ((TempSensor*)pv)->sensor();
}

inline void setSensor(DeviceFunction f, void** ppv, BasicTempSensor* sensor) {
	if (isBasicSensor(f)) 
		*ppv = sensor;
	else
		((TempSensor*)*ppv)->setSensor(sensor);
}

/**
 * Removes an installed device.
 * /param config The device to remove. The fields that are used are
 *		chamber, beer, hardware and function.
 */
void DeviceManager::uninstallDevice(DeviceConfig& config)
{
	DeviceType dt = deviceType(config.deviceFunction);
	void** ppv = deviceTarget(config);	
	if (ppv==NULL)
		return;
	
	BasicTempSensor* s;
	switch(dt) {
		case DEVICETYPE_NONE:
			break;
		case DEVICETYPE_TEMP_SENSOR:
			// sensor may be wrapped in a TempSensor class, or may stand alone.
			s = &unwrapSensor(config.deviceFunction, *ppv);
			if (s!=&defaultTempSensor) {
				setSensor(config.deviceFunction, ppv, &defaultTempSensor);
				DEBUG_MSG(PSTR("Uninstalling temp sensor f=%d"), config.deviceFunction);
				delete s;
			}
			break;
		case DEVICETYPE_SWITCH_ACTUATOR:
			if (*ppv!=&defaultActuator) {
				DEBUG_MSG(PSTR("Uninstalling actuator f=%d"), config.deviceFunction);
				delete (Actuator*)*ppv;
				*ppv = &defaultActuator;
			}
			break;
		case DEVICETYPE_SWITCH_SENSOR:
			if (*ppv!=&defaultSensor) {
				DEBUG_MSG(PSTR("Uninstalling sensor f=%d"), config.deviceFunction);
				delete (SwitchSensor*)*ppv;
				*ppv = &defaultSensor;
			}
			break;
	}		
}

/**
 * Creates and installs a device in the current chamber. 
 */
void DeviceManager::installDevice(DeviceConfig& config)
{	
	DeviceType dt = deviceType(config.deviceFunction);
	void** ppv = deviceTarget(config);
	if (ppv==NULL || config.hw.deactivate)
		return;
		
	BasicTempSensor* s;
	TempSensor* ts;
	switch(dt) {
		case DEVICETYPE_NONE:
			break;
		case DEVICETYPE_TEMP_SENSOR:
			DEBUG_MSG(PSTR("Installing temp sensor f=%d"), config.deviceFunction);
			// sensor may be wrapped in a TempSensor class, or may stand alone.
			s = (BasicTempSensor*)createDevice(config, dt);
#if BREWPI_DEBUG
			if (*ppv==NULL)
				DEBUG_MSG(PSTR("*** OUT OF MEMORY for device f=%d"), config.deviceFunction);
#endif
			if (isBasicSensor(config.deviceFunction)) {
				s->init();
				*ppv = s;				
			}
			else {
				ts = ((TempSensor*)*ppv);
				ts->setSensor(s);
				ts->init();
			}
			break;
		case DEVICETYPE_SWITCH_ACTUATOR:
		case DEVICETYPE_SWITCH_SENSOR:
			DEBUG_MSG(PSTR("Installing device f=%d"), config.deviceFunction);
			*ppv = createDevice(config, dt);
#if BREWPI_DEBUG
			if (*ppv==NULL)
				DEBUG_MSG(PSTR("*** OUT OF MEMORY for device f=%d"), config.deviceFunction);
#endif			
			break;
	}	
}	

struct DeviceDefinition {
	int8_t id;
	int8_t chamber;
	int8_t beer;
	int8_t deviceFunction;
	int8_t deviceHardware;
	int8_t pinNr;
	int8_t invert;	
	int8_t pio;
	int8_t deactivate;
	int8_t calibrationAdjust;
	DeviceAddress address;
		
	/**
	 * Lists the first letter of the key name for each attribute.
	 */
	static const char ORDER[12];
};

// the special cases are placed at the end. All others should map directly to an int8_t via atoi().
const char DeviceDefinition::ORDER[12] = "icbfhpxndja";

const char DEVICE_ATTRIB_INDEX = 'i';
const char DEVICE_ATTRIB_CHAMBER = 'c';
const char DEVICE_ATTRIB_BEER = 'b';
const char DEVICE_ATTRIB_FUNCTION = 'f';
const char DEVICE_ATTRIB_HARDWARE = 'h';
const char DEVICE_ATTRIB_PIN = 'p';
const char DEVICE_ATTRIB_INVERT = 'x';
const char DEVICE_ATTRIB_DEACTIVATED = 'd';
const char DEVICE_ATTRIB_ADDRESS = 'a';
#if BREWPI_DS2413
const char DEVICE_ATTRIB_PIO = 'n';
#endif
const char DEVICE_ATTRIB_CALIBRATEADJUST = 'j';	// value to add to temp sensors to bring to correct temperature

const char DEVICE_ATTRIB_VALUE = 'v';		// print current values
const char DEVICE_ATTRIB_WRITE = 'w';		// write value to device

const char DEVICE_ATTRIB_TYPE = 't';


inline int8_t indexOf(const char* s, char c)
{
	char c2;
	int8_t idx = -1;
	while ((c2=s[++idx]))
	{
		if (c==c2) 
			return idx;
	}
	return -1;
}

void handleDeviceDefinition(const char* key, const char* val, void* pv)
{
	DeviceDefinition* def = (DeviceDefinition*) pv;
	DEBUG_MSG(PSTR("deviceDef %s:%s"), key, val);
	
	// the characters are listed in the same order as the DeviceDefinition struct.
	int8_t idx = indexOf(DeviceDefinition::ORDER, key[0]);
	if (key[0]==DEVICE_ATTRIB_ADDRESS)
		parseBytes(def->address, val, 8);
	else if (key[0]==DEVICE_ATTRIB_CALIBRATEADJUST) {
		def->calibrationAdjust = fixed4_4(stringToTempDiff(val)>>5);
	}		
	else if (idx>=0) 
		((uint8_t*)def)[idx] = (uint8_t)atol(val);
}

bool inRangeUInt8(uint8_t val, uint8_t min, int8_t max) {
	return min<=val && val<=max;
}

bool inRangeInt8(int8_t val, int8_t min, int8_t max) {
	return min<=val && val<=max;
}


/**
 * Updates the device definition. Only changes that result in a valid device, with no conflicts with other devices
 * are allowed. 
 */
void DeviceManager::parseDeviceDefinition(Stream& p)
{	
	static DeviceDefinition dev;
	fill((int8_t*)&dev, sizeof(dev));
	
	piLink.parseJson(&handleDeviceDefinition, &dev);
	
	if (!inRangeInt8(dev.id, 0, MAX_DEVICE_SLOT))			// no device id given, or it's out of range, can't do anything else.
		return;
	
	// save the original device so we can revert
	DeviceConfig target;
	DeviceConfig original;
	
	// todo - should ideally check if the eeprom is correctly initialized.
	eepromManager.fetchDevice(original, dev.id);
	memcpy(&target, &original, sizeof(target));
	
	if (dev.chamber>=0) {
		target.chamber = dev.chamber;
	}				
	if (dev.beer>=0) {
		target.beer = dev.beer;
	}		
	if (dev.deviceFunction>=0) {
		target.deviceFunction = DeviceFunction(dev.deviceFunction);
	}		
	if (dev.deviceHardware>=0) {		
		target.deviceHardware = DeviceHardware(dev.deviceHardware);		
	}

	if (dev.pinNr>=0) 
		target.hw.pinNr = dev.pinNr;
	
	if (dev.calibrationAdjust!=-1)		// since this is a union, it also handles pio for 2413 sensors
		target.hw.calibration = dev.calibrationAdjust;

	if (dev.invert>=0) 
		target.hw.invert = dev.invert;
		
	if (dev.address[0]!=0xFF)	// first byte is family identifier. I don't have a complete list, but so far 0xFF is not used.
		memcpy(target.hw.address, dev.address, 8);

	if (dev.deactivate>=0)
		target.hw.deactivate = dev.deactivate;
	
	// setting function to none clears all other fields.
	if (target.deviceFunction==DEVICE_NONE) {
		clear((uint8_t*)&target, sizeof(target));
	}
	
	bool valid = isDeviceValid(target, original, dev.id);
	DeviceConfig* print = &original;
	if (valid) {		
		print = &target;
		// remove the device associated with the previous function
		uninstallDevice(original);
		// also remove any existing device for the new function, since install overwrites any existing definition.
		uninstallDevice(target);
		installDevice(target);		
		eepromManager.storeDevice(target, dev.id);		
	}	
	else {
		DEBUG_MSG("Device definition update spec is not valid");
	}
	
	deviceManager.beginDeviceOutput();
	deviceManager.printDevice(dev.id, *print, NULL, p);
}

/**
 * Determines if a given device definition is valid.
 * chamber/beer must be within bounds
 * device function must match the chamber/beer spec, and must not already be defined for the same chamber/beer combination
 * device hardware type must be applicable with the device function
 * pinNr must be unique for digital pin devices?
 * pinNr must be a valid OneWire bus for one wire devices.
 * for onewire temp devices, address must be unique.
 * for onewire ds2413 devices, address+pio must be unique.
 */
bool DeviceManager::isDeviceValid(DeviceConfig& config, DeviceConfig& original, uint8_t deviceIndex)
{
#if 1
	/* Implemented checks to ensure the system will not crash when supplied with invalid data.
	   More refined checks that may cause confusing results are not yet implemented. See todo below. */
	
	/* chamber and beer within range.*/
	if (!inRangeUInt8(config.chamber, 0, EepromFormat::MAX_CHAMBERS))
	{
		DEBUG_MSG(PSTR("Invalid chamber id %d"), config.chamber);
		return false;
	}

	/* 0 is allowed - represents a chamber device not assigned to a specific beer */
	if (!inRangeUInt8(config.beer, 0, ChamberBlock::MAX_BEERS))
	{
		DEBUG_MSG(PSTR("Invalid beer id %d"), config.beer);
		return false;
	}

	if (!inRangeUInt8(config.deviceFunction, 0, DEVICE_MAX-1))
	{
		DEBUG_MSG(PSTR("Invalid device function %d"), config.deviceFunction);
		return false;
	}

	DeviceOwner owner = deviceOwner(config.deviceFunction);
	if (!((owner==DEVICE_OWNER_BEER && config.beer) || (owner==DEVICE_OWNER_CHAMBER && config.chamber) 
		|| (owner==DEVICE_OWNER_NONE && !config.beer && !config.chamber))) 
	{
		DEBUG_MSG(PSTR("Invalid config for device owner type %d beer=%d chamber=%d"), owner, config.beer, config.chamber);	
		return false;
	}
		
	// todo - find device at another index with the same chamber/beer/function spec.
	// with duplicate function defined for the same beer, that they will both try to create/delete the device in the target location. 
	// The highest id will win.	
	DeviceType dt = deviceType(config.deviceFunction);
	if (!isAssignable(dt, config.deviceHardware)) {
		DEBUG_MSG(PSTR("Cannot assign device type %d to hardware %d"), dt, config.deviceHardware);
		return false;
	}
			
	// todo - check pinNr uniqueness for direct digital I/O devices?
	
	/* pinNr for a onewire device must be a valid bus. While this won't cause a crash, it's a good idea to validate this. */
	if (isOneWire(config.deviceHardware)) {	
		if (!oneWireBus(config.hw.pinNr)) {
			DEBUG_MSG(PSTR("Device is onewire but pin %d is not configured as a onewire bus"), config.hw.pinNr);
			return false;
		}
	}
	else {		// regular pin device
		// todo - could verify that the pin nr corresponds to enumActuatorPins/enumSensorPins		
	}
	
#endif
	// todo - for onewire temp, ensure address is unique	
	// todo - for onewire 2413 check address+pio nr is unique
	return true;
}	

void printAttrib(Print& p, char c, int8_t val, bool first=false) 
{		
	if (!first)
		p.print(',');
	p.print(c);
	p.print(':');
	p.print(val);
}

inline bool hasInvert(DeviceHardware hw)
{
	return hw==DEVICE_HARDWARE_PIN
#if BREWPI_DS2413	
	|| hw==DEVICE_HARDWARE_ONEWIRE_2413 
#endif	
	;
}

inline bool hasOnewire(DeviceHardware hw)
{
	return 
#if BREWPI_DS2413
	hw==DEVICE_HARDWARE_ONEWIRE_2413 || 
#endif	
	hw==DEVICE_HARDWARE_ONEWIRE_TEMP;
}

void DeviceManager::printDevice(device_slot_t slot, DeviceConfig& config, const char* value, Print& p)
{	
	char buf[17];

	DeviceType dt = deviceType(config.deviceFunction);
	if (!firstDeviceOutput) {
		p.print('\n'); p.print(',');
	}
	firstDeviceOutput = false;
	p.print('{');
	printAttrib(p, DEVICE_ATTRIB_INDEX, slot, true);
	printAttrib(p, DEVICE_ATTRIB_TYPE, dt);
	
	printAttrib(p, DEVICE_ATTRIB_CHAMBER, config.chamber);
	printAttrib(p, DEVICE_ATTRIB_BEER, config.beer);
	printAttrib(p, DEVICE_ATTRIB_FUNCTION, config.deviceFunction);	
	printAttrib(p, DEVICE_ATTRIB_HARDWARE, config.deviceHardware);		
	printAttrib(p, DEVICE_ATTRIB_DEACTIVATED, config.hw.deactivate);
	printAttrib(p, DEVICE_ATTRIB_PIN, config.hw.pinNr);
	if (value && *value) {
		p.print(",v:");
		p.print(value);
	}
	if (hasInvert(config.deviceHardware))	
		printAttrib(p, DEVICE_ATTRIB_INVERT, config.hw.invert);
	
	if (hasOnewire(config.deviceHardware)) {
		p.print(",a:\"");
		printBytes(config.hw.address, 8, buf);
		p.print(buf);
		p.print('"');
	}	
#if BREWPI_DS2413		
	if (config.deviceHardware==DEVICE_HARDWARE_ONEWIRE_2413) {
		printAttrib(p, DEVICE_ATTRIB_PIO, config.hw.pio);		
	}
#endif	
	if (config.deviceHardware==DEVICE_HARDWARE_ONEWIRE_TEMP) {
		tempDiffToString(buf, fixed7_9(config.hw.calibration)<<5, 3, 8);
		p.print(",j:");
		p.print(buf);
	}
	p.print('}');
}	
	
bool DeviceManager::allDevices(DeviceConfig& config, uint8_t deviceIndex)
{	
	return eepromManager.fetchDevice(config, deviceIndex);
}	

void parseBytes(uint8_t* data, const char* s, uint8_t len) {
	char c;
	while ((c=*s++)) {
		uint8_t d = (c>='A' ? c-'A'+10 : c-'0')<<4;
		c = *s++;
		d |= (c>='A' ? c-'A'+10 : c-'0');
		*data++ = d;
	}
}

void printBytes(uint8_t* data, uint8_t len, char* buf) // prints 8-bit data in hex
{
	for (int i=0; i<len; i++) {
		uint8_t b = (data[i] >> 4) & 0x0f;
		*buf++ = (b>9 ? b-10+'A' : b+'0');
		b = data[i] & 0x0f;
		*buf++ = (b>9 ? b-10+'A' : b+'0');
	}
	*buf = 0;
}

void DeviceManager::OutputEnumeratedDevices(DeviceConfig* config, void* pv)
{
	DeviceOutput* out = (DeviceOutput*)pv;
	printDevice(out->slot, *config, out->value, *out->pp);
}

bool DeviceManager::enumDevice(DeviceDisplay& dd, DeviceConfig& dc, uint8_t idx)
{
	if (dd.id==-1)
		return (dd.empty || dc.deviceFunction);	// if enumerating all devices, honor the unused request param
	else
		return (dd.id==idx);						// enumerate only the specific device requested
}

struct EnumerateHardware
{
	int8_t hardware;		// restrict the types of devices requested
	int8_t pin;				// pin to search
	int8_t values;			// fetch values for the devices.
	int8_t unused;			// 0 don't care about unused state, 1 unused only.
	int8_t function;		// restrict to devices that can be used with this function
};

void handleHardwareSpec(const char* key, const char* val, void* pv)
{
	EnumerateHardware* h = (EnumerateHardware*)pv;
	DEBUG_MSG(PSTR("hardwareSpec %s:%s"), key, val);
	
	int8_t idx = indexOf("hpvuf", key[0]);
	if (idx>=0) {
		*((int8_t*)h+idx) = atol(val);
	}			
}

inline bool matchAddress(uint8_t* a1, uint8_t* a2, uint8_t count) {
	while (count-->0) {
		if (a1[count]!=a2[count])
			return false;
	}
	return true;
}

/**
 * Find a device based on it's location.
 * A device's location is:
 *   pinNr  for simple digital pin devices
 *   pinNr+address for one-wire devices
 *   pinNr+address+pio for 2413
 */
device_slot_t findHardwareDevice(DeviceConfig& find)
{
	DeviceConfig config;
	for (device_slot_t slot= 0; deviceManager.allDevices(config, slot); slot++) {
		if (find.deviceHardware==config.deviceHardware) {
			bool match = true;
			switch (find.deviceHardware) {
#if BREWPI_DS2413
				case DEVICE_HARDWARE_ONEWIRE_2413:
					match &= find.hw.pio==config.hw.pio;					
					// fall through
#endif					
				case DEVICE_HARDWARE_ONEWIRE_TEMP:
					match &= matchAddress(find.hw.address, config.hw.address, 8);					
					// fall through
				case DEVICE_HARDWARE_PIN:
					match &= find.hw.pinNr==config.hw.pinNr;
				default:	// this should not happen - if it does the device will be returned as matching.
					break;
			}
			if (match)
				return slot;
		}
	}
	return INVALID_SLOT;
}

inline void DeviceManager::readTempSensorValue(DeviceConfig::Hardware hw, char* out)
{
	OneWire* bus = oneWireBus(hw.pinNr);
	OneWireTempSensor sensor(bus, hw.address, 0);		// NB: this value is uncalibrated, since we don't have the calibration offset until the device is configured
	fixed7_9 value = sensor.init();	
	fixedPointToString(out, value, 3, 9);
}

void DeviceManager::handleEnumeratedDevice(DeviceConfig& config, EnumerateHardware& h, EnumDevicesCallback callback, DeviceOutput& out)
{
	if (h.function && !isAssignable(deviceType(DeviceFunction(h.function)), config.deviceHardware)) 
		return; // device not applicable for required function
	
	DEBUG_MSG(PSTR("Handling device"));
	out.slot = findHardwareDevice(config);
	DEBUG_MSG(PSTR("Matching device at slot %d"), out.slot);
	
	if (isDefinedSlot(out.slot)) {
		if (h.unused)	// only list unused devices, and this one is already used
			return;
		// display the actual matched value
		deviceManager.allDevices(config, out.slot);
	}
	
	out.value[0] = 0;
	if (h.values) {
		DEBUG_MSG(PSTR("Fetching device value"));
		switch (config.deviceHardware) {
			case DEVICE_HARDWARE_ONEWIRE_TEMP:
				readTempSensorValue(config.hw, out.value);
				break;
			// unassigned pins could be input or output so we can't determine any other details from here.
			// values can be read once the pin has been assigned a function
			default:
				break;							
		}
	}	
	DEBUG_MSG(PSTR("Passing device to callback"));
	callback(&config, &out);
}

void DeviceManager::enumeratePinDevices(EnumerateHardware& h, EnumDevicesCallback callback, DeviceOutput& output) 
{
	DeviceConfig config;
	clear((uint8_t*)&config, sizeof(config));
	config.deviceHardware = DEVICE_HARDWARE_PIN;
	
	int8_t pin;	
	for (uint8_t count=0; (pin=deviceManager.enumerateActuatorPins(count))>=0; count++) {
		if (h.pin!=-1 && h.pin!=pin)
			continue;
		config.hw.pinNr = pin;
		handleEnumeratedDevice(config, h, callback, output);
	}	
	
	for (uint8_t count=0; (pin=deviceManager.enumerateSensorPins(count))>=0; count++) {
		if (h.pin!=-1 && h.pin!=pin)
			continue;
		config.hw.pinNr = pin;
		handleEnumeratedDevice(config, h, callback, output);
	}	
}

void DeviceManager::enumerateOneWireDevices(EnumerateHardware& h, EnumDevicesCallback callback, DeviceOutput& output)
{		
	int8_t pin;	
	DEBUG_MSG(PSTR("Enumerating one-wire devices"));
	for (uint8_t count=0; (pin=deviceManager.enumOneWirePins(count))>=0; count++) {
		DeviceConfig config;
		clear((uint8_t*)&config, sizeof(config));
		if (h.pin!=-1 && h.pin!=pin)
			continue;
		config.hw.pinNr = pin;
		DEBUG_MSG(PSTR("Enumerating one-wire devices on pin %d"), pin);				
		OneWire* wire = oneWireBus(pin);	
		if (wire!=NULL) {
			wire->reset_search();
			while (wire->search(config.hw.address)) {
				// hardware device type from onewire family ID
				switch (config.hw.address[0]) {
		#if BREWPI_DS2413
					case DS2413_FAMILY_ID:
						config.deviceHardware = DEVICE_HARDWARE_ONEWIRE_2413;
						break;
		#endif				
					case DS18B20MODEL:
						config.deviceHardware = DEVICE_HARDWARE_ONEWIRE_TEMP;
						break;				
					default:
						config.deviceHardware = DEVICE_HARDWARE_NONE;
				}
		
				switch (config.deviceHardware) {
		#if BREWPI_DS2413
					// for 2408 this will require iterating 0..7
					case DEVICE_HARDWARE_ONEWIRE_2413:
						// enumerate each pin separately
						for (uint8_t i=0; i<2; i++) {
							config.hw.pio = i;
							handleEnumeratedDevice(config, h, callback, output);
						}
						break;
		#endif				
					default:
						handleEnumeratedDevice(config, h, callback, output);
				}
			}
		}
		DEBUG_MSG(PSTR("Enumerating one-wire devices on pin %d complete"), pin);
	}	
}

void DeviceManager::enumerateHardware( Stream& p )
{
	EnumerateHardware spec;
	// set up defaults
	spec.unused = 0;			// list all devices
	spec.values = 0;			// don't list values
	spec.pin = -1;				// any pin
	spec.hardware = -1;			// any hardware
	spec.function = 0;			// no function restriction
	
	piLink.parseJson(handleHardwareSpec, &spec);	
	DeviceOutput out;
	out.pp = &p;

	DEBUG_MSG(PSTR("Enumerating Hardware"));
	firstDeviceOutput = true;
	if (spec.hardware==-1 || isOneWire(DeviceHardware(spec.hardware))) {
		enumerateOneWireDevices(spec, OutputEnumeratedDevices, out);
	}
	if (spec.hardware==-1 || isDigitalPin(DeviceHardware(spec.hardware))) {
		enumeratePinDevices(spec, OutputEnumeratedDevices, out);
	}
	
	DEBUG_MSG(PSTR("Enumerating Hardware Complete"));
}


void HandleDeviceDisplay(const char* key, const char* val, void* pv)
{
	DeviceDisplay& dd = *(DeviceDisplay*)pv;
	//DEBUG_MSG(PSTR("DeviceDisplay %s:%s"), key, val);
	
	int8_t idx = indexOf("irwe", key[0]);
	if (idx>=0) {
		*((int8_t*)&dd+idx) = atol(val);
	}	
}

void UpdateDeviceState(DeviceDisplay& dd, DeviceConfig& dc, char* val)
{
	DeviceType dt = deviceType(dc.deviceFunction);
	if (dt==DEVICETYPE_NONE)
		return;
		
	void** ppv = deviceTarget(dc);
	if (ppv==NULL)
		return;

	if (dd.write>=0 && dt==DEVICETYPE_SWITCH_ACTUATOR) {
		// write value to a specific device. For now, only actuators are relevant targets
		DEBUG_MSG(PSTR("setting activator state %d"), dd.write!=0);
		((Actuator*)*ppv)->setActive(dd.write!=0);
	}
	else if (dd.value==1) {		// read values 
		if (dt==DEVICETYPE_SWITCH_SENSOR) {
			itoa(((SwitchSensor*)*ppv)->sense()!=0, val, 10);
		}
		else if (dt==DEVICETYPE_TEMP_SENSOR) {
			BasicTempSensor& s = unwrapSensor(dc.deviceFunction, *ppv);
			fixed7_9 temp = s.read();
			fixedPointToString(val, temp, 3, 9);
		}
		// todo - should it be possible to read the last set state on an activator?
	}
}

void DeviceManager::listDevices(Stream& p) {
	DeviceConfig dc;
	DeviceDisplay dd;
	fill((int8_t*)&dd, sizeof(dd));
	dd.empty = 0;
	piLink.parseJson(HandleDeviceDisplay, (void*)&dd);
	deviceManager.beginDeviceOutput();
	for (device_slot_t idx=0; deviceManager.allDevices(dc, idx); idx++) {
		if (deviceManager.enumDevice(dd, dc, idx))
		{
			char val[10];
			val[0] = 0;
			UpdateDeviceState(dd, dc, val);
			deviceManager.printDevice(idx, dc, val, p);			
		}
	}	
}
