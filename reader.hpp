#pragma once

#include <cstdint>
#include <vector>
#include <bitset>

#include <QByteArray>
#include <QHostAddress>
#include <QTcpSocket>

enum class InputMode { monopolar, differential, bipolar };
enum class Connector { in1, in2, in3, in4, in5, in6, in7, in8, multin1, multin2, multin3, multin4, auxin };

struct ReaderParameters {
	QHostAddress ip_address;
	uint16_t port;
	uint16_t fs; // 512, 2048, 5120, 10240
	uint16_t samples_per_block; // 2, 4, 6, 12
	uint16_t active_inputs; // 1 - 4
	uint16_t n_channels; // 120, 216, 312, 408
	bool decim; // true, false

	bool in_active[8];
	bool multin_active[4];
	bool aux_active;

	InputMode in_mode[8]; // InputMode:monopolar, InputMode:differential (bipolar is not supported)
	InputMode multin_mode[4]; // InputMode:monopolar, InputMode:differential (bipolar is not supported)

	uint16_t in_hp[8]; // 0 (= 0.3), 10, 100, 200
	uint16_t multin_hp[4]; // 0 (= 0.3), 10, 100, 200

	uint16_t in_lp[8]; // 130, 500, 900, 4400
	uint16_t multin_lp[4]; // 130, 500, 900, 4400

	Connector source_connector; // Connector:in1 - Connector:auxin
	uint16_t source_channel; // 1 - 64
	uint16_t output_gain; // 1, 2, 4, 16
};

// Most recording device APIs provide some sort of handle to a device and
// functions to query the state, read data and put it in a buffer etc.
//
// This is a very simple example to demonstrate how to integrate it with LSL.
// The provided functions are:
//
// - the constructor
// - the destructor
// - `getData` with the buffer as one output parameter and the status as return value
// - `getStatus` to check if everything's ok
class Reader {
public:
	explicit Reader(ReaderParameters params);
	~Reader() noexcept;
	bool getData(std::vector<int16_t> &buffer);
	QString error_msg;

private:
	template <class T>
	std::bitset<8> binaryEncodingOfIndex(T item, std::vector<T> sorted_list) const;
	std::bitset<8> crc(const QByteArray& configuration) const;
	QTcpSocket *socket = nullptr;
	void printConfiguration(const QByteArray& configuration) const;
	char* temp_buffer = nullptr;

	static const uint16_t temp_buffer_size = 10240;
	static const uint16_t connection_timeout = 1000; // [ms]
};
