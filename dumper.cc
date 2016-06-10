#include <arpa/inet.h>
#include <iostream>
#include "dumper.h"
#include "util.h"

using namespace std;

namespace bcm2dump {
namespace {

template<class T> T hex_cast(const std::string& str)
{
	return lexical_cast<T>(str, 16);
}

class parsing_dumper : public dumper
{
	public:
	virtual ~parsing_dumper()
	{ cleanup(); }

	virtual string read_chunk(uint32_t offset, uint32_t length) override final
	{ return read_chunk_impl(offset, length, 0); }

	virtual uint32_t chunk_size() const override
	{ return 2048; }

	protected:
	virtual void init() {}
	virtual void cleanup() {}
	virtual string read_chunk_impl(uint32_t offset, uint32_t length, uint32_t retries);
	virtual void do_read_chunk(uint32_t offset, uint32_t length) = 0;
	virtual bool is_ignorable_line(const string& line) = 0;
	virtual string parse_chunk_line(const string& line, uint32_t offset) = 0;

	private:
	bool m_inited = false;
};

string parsing_dumper::read_chunk_impl(uint32_t offset, uint32_t length, uint32_t retries)
{
	if (!m_inited) {
		init();
		m_inited = true;
	}

	do_read_chunk(offset, length);

	string line, linebuf, chunk;
	uint32_t pos = offset;

	while (chunk.size() < length && m_intf->pending()) {
		line = trim(m_intf->readln(100));

		if (is_ignorable_line(line)) {
			continue;
		} else {
			string linebuf = parse_chunk_line(line, pos);   
			if (!linebuf.empty()) {
				pos += linebuf.size();
				chunk += linebuf;
			} else {
				break;
			}
		}
	}

	if (chunk.size() != length) {
		if (retries >= 2) {
			throw runtime_error("failed to read chunk (@" + to_hex(offset)
					+ ", " + to_string(length) + "); line was\n'" + line + "'");
		}
			
		// TODO log
		return read_chunk_impl(offset, length, retries + 1);
	}

	return chunk;
}

class bfc_ram_dumper : public parsing_dumper
{
	public:
	virtual void do_read_chunk(uint32_t offset, uint32_t length) override;
	virtual bool is_ignorable_line(const string& line) override;
	virtual string parse_chunk_line(const string& line, uint32_t offset) override;
};

void bfc_ram_dumper::do_read_chunk(uint32_t offset, uint32_t length)
{
	m_intf->runcmd("/read_memory -s 4 -n " + to_string(length) + " " + to_string(offset));
}

bool bfc_ram_dumper::is_ignorable_line(const string& line)
{
	if (line.size() >= 65) {
		if (line[8] == ':' || line.substr(48, 3) == " | ") {
			return false;
		}
	}

	return true;
}

string bfc_ram_dumper::parse_chunk_line(const string& line, uint32_t offset)
{
	try {
		uint32_t val = hex_cast<uint32_t>(line.substr(0, 8));
		if (val != offset) {
			return "";
		}

		string linebuf;

		for (unsigned i = 0; i < 4; ++i) {
			val = hex_cast<uint32_t>(line.substr((i + 1) * 10, 8));
			val = htonl(val);
			linebuf += string(reinterpret_cast<char*>(&val), 4);
		}

		return linebuf;
	} catch (const bad_lexical_cast& e) {
		return "";
	}
}

class bfc_flash_dumper : public parsing_dumper
{
	protected:
	virtual void init() override;
	virtual void cleanup() override;

	virtual void do_read_chunk(uint32_t offset, uint32_t length) override;
	virtual bool is_ignorable_line(const string& line) override;
	virtual string parse_chunk_line(const string& line, uint32_t offset) override;
};

void bfc_flash_dumper::init()
{
	if (m_partition.empty()) {
		throw runtime_error("cannot dump without a partition name");
	}

	cleanup();
	m_intf->runcmd("/flash/open " + m_partition);

	bool opened = false;

	while (m_intf->pending()) {
		string line = m_intf->readln();
		if (line.empty()) {
			break;
		}

		if (line.find("opened") != string::npos) {
			opened = true;
		}
	}

	if (!opened) {
		throw runtime_error("failed to open partition " + m_partition);
	}
}

void bfc_flash_dumper::cleanup()
{
	m_intf->runcmd("/flash/close");
	while (m_intf->pending()) {
		m_intf->readln();
	}
}

void bfc_flash_dumper::do_read_chunk(uint32_t offset, uint32_t length)
{
	m_intf->runcmd("/flash/readDirect " + to_string(length) + " " + to_string(offset));
}

bool bfc_flash_dumper::is_ignorable_line(const string& line)
{
	if (line.size() >= 53) {
		if (line.substr(11, 3) == "   " && line.substr(25, 3) == "   ") {
			return false;
		}
	}	

	return true;
}

string bfc_flash_dumper::parse_chunk_line(const string& line, uint32_t offset)
{
	try {
		string linebuf;

		for (unsigned i = 0; i < 16; ++i) {
			// don't change this to uint8_t
			uint32_t val = hex_cast<uint32_t>(line.substr(i * 3 + (i / 4) * 2, 2));
			if (val > 0xff) {
				return "";
			}

			linebuf += char(val);
		}

		return linebuf;
	} catch (const bad_lexical_cast& e) {
		return "";
	}
}

class bootloader_ram_dumper : public parsing_dumper
{
	public:
	virtual uint32_t chunk_size() const override
	{ return 4; }


	protected:
	virtual void init() override;
	virtual void cleanup() override;

	virtual void do_read_chunk(uint32_t offset, uint32_t length) override;
	virtual bool is_ignorable_line(const string& line) override;
	virtual string parse_chunk_line(const string& line, uint32_t offset) override;
};

void bootloader_ram_dumper::init()
{
	m_intf->write("r");
}

void bootloader_ram_dumper::cleanup()
{
	m_intf->writeln();
}

void bootloader_ram_dumper::do_read_chunk(uint32_t offset, uint32_t length)
{
	while (m_intf->pending()) {
		string line = m_intf->readln();
		if (line.find("address:") != string::npos) {
			m_intf->writeln("0x" + to_hex(offset));
			return;
		}
	}

	throw runtime_error("unexpected status");
}

bool bootloader_ram_dumper::is_ignorable_line(const string& line)
{
	if (contains(line, "Value at") || contains(line, "(hex)")) {
		return false;
	}

	return true;
}

string bootloader_ram_dumper::parse_chunk_line(const string& line, uint32_t offset)
{
	while (m_intf->pending()) {
		string line = m_intf->readln();
		if (line.find("Value at") == 0) {
			try {
				if (offset != hex_cast<uint32_t>(line.substr(9, 8))) {
					break;
				}

				uint32_t val = hex_cast<uint32_t>(line.substr(19, 8));
				val = htonl(val);
				
				return string(reinterpret_cast<char*>(&val), 4);
			} catch (const bad_lexical_cast& e) {
				break;
			}
		}
	}

	return "";
}

class bootloader_fast_dumper : public parsing_dumper
{
	public:
	virtual uint32_t chunk_size() const override
	{ return 2048; }

	protected:
	virtual void init() override;
	virtual void cleanup() override;

	virtual void do_read_chunk(uint32_t offset, uint32_t length) override;
	virtual bool is_ignorable_line(const string& line) override;
	virtual string parse_chunk_line(const string& line, uint32_t offset) override;
};

template<class T> dumper::sp get_dumper(const interface::sp& intf)
{
	dumper::sp ret = make_shared<T>();
	ret->set_interface(intf);
	return ret;
}
}

void dumper::dump(uint32_t offset, uint32_t length, std::ostream& os)
{
	if (offset % offset_alignment()) {
		throw invalid_argument("offset not aligned to " + to_string(offset_alignment()) + " bytes");
	}

	if (length % length_alignment()) {
		throw invalid_argument("length not aligned to " + to_string(length_alignment()) + " bytes");
	}

	while (length) {
		uint32_t n = min(chunk_size(), length);
		string chunk = read_chunk(offset, n);

		if (chunk.size() != n) {
			throw runtime_error("unexpected chunk length: " + to_string(chunk.size()));
		}

		os.write(chunk.c_str(), chunk.size());

		length -= n;
		offset += n;
	}
}

string dumper::dump(uint32_t offset, uint32_t length)
{
	ostringstream ostr;
	dump(offset, length, ostr);
	return ostr.str();
}

dumper::sp dumper::get_bfc_ram(const shared_ptr<interface>& intf)
{
	return get_dumper<bfc_ram_dumper>(intf);
}

dumper::sp dumper::get_bfc_flash(const shared_ptr<interface>& intf)
{
	return get_dumper<bfc_flash_dumper>(intf);
}
}
