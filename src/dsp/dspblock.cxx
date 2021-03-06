/*
 * WebRadio web-based Software Defined Radio
 *
 * Copyright (C) 2013 Mike Stirling
 *
 * This file is part of WebRadio (http://www.mike-stirling.com/webradio)
 *
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <vector>
#include <string>
#include <algorithm>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "debug.h"
#include "dspblock.h"

DspBlock::DspBlock(const string &name, const string &type) :
			_outputSampleRate(DEFAULT_SAMPLE_RATE),
			_outputChannels(DEFAULT_CHANNELS),
			_name(name),
			_type(type),
			_inputSampleRate(DEFAULT_SAMPLE_RATE),
			_inputChannels(DEFAULT_CHANNELS),
			_decimation(1), _interpolation(1),
#ifdef DSPBLOCK_PROFILE
			_totalNanoseconds(0),
			_totalIn(0), _totalOut(0),
#endif
			_isRunning(false)
{

}

DspBlock::~DspBlock()
{
	if (_isRunning)
		stop();
}

void DspBlock::connect(DspBlock *block)
{
	if (_isRunning) {
		/* Start downstream */
		block->start();
	}

	/* Hook up */
	if (find(consumers.begin(), consumers.end(), block) != consumers.end()) {
		LOG_ERROR("Block %s:%s already connected to %s:%s\n",
				block->type().c_str(), block->name().c_str(),
				this->type().c_str(), this->name().c_str());
		return;
	}

	consumers.push_back(block);
	LOG_DEBUG("Added block %s:%s as consumer of %s:%s\n",
			block->type().c_str(), block->name().c_str(),
			this->type().c_str(), this->name().c_str());
}

void DspBlock::disconnect(DspBlock *block)
{
	if (_isRunning) {
		/* Stop downstream */
		block->stop();
	}

	/* Un-hook */
	consumers.erase(remove(consumers.begin(), consumers.end(), block),
			consumers.end());
	LOG_DEBUG("Removed block %s:%s as consumer of %s:%s\n",
			block->type().c_str(), block->name().c_str(),
			this->type().c_str(), this->name().c_str());
}

#ifdef DSPBLOCK_PROFILE
uint64_t DspBlock::nsPerFrameAll() const
{
	uint64_t total;

	total = nsPerFrameOne();
	LOG_DEBUG("%s:%s %" PRIu64 " ns/frame\n", type().c_str(), name().c_str(), total);
	for (vector<DspBlock*>::const_iterator it = consumers.begin(); it != consumers.end(); ++it)
		total += (*it)->nsPerFrameAll();
	return total;
}
#endif

bool DspBlock::start()
{
	/* Defaults, primarily for sinks to avoid breaking buffer calcs */
	_outputSampleRate = _inputSampleRate;
	_outputChannels = _inputChannels;

	/* Start this block - may update its output sample rate */
	LOG_DEBUG("Starting block %s:%s\n", this->type().c_str(), this->name().c_str());
	if (!init()) {
		LOG_ERROR("Block %s:%s failed to initialise\n", this->type().c_str(), this->name().c_str());
		return false;
	}
	/* Validate new sample rate and calculate decimation/interpolation rate */
	if (_inputSampleRate >= _outputSampleRate) {
		_decimation = _inputSampleRate / _outputSampleRate;
		_interpolation = 1;
	} else {
		_decimation = 1;
		_interpolation = _outputSampleRate / _inputSampleRate;
	}
	if (_inputSampleRate * _interpolation / _decimation != _outputSampleRate) {
		LOG_ERROR("Sample rates must be integer related\n");
		deinit();
		return false;
	}
#ifdef DSPBLOCK_PROFILE
	_totalIn = _totalOut = 0;
	_totalNanoseconds = 0;
#endif
	_isRunning = true;

	/* Configure and start downstream */
	vector<DspBlock*>::iterator it;
	for (it = consumers.begin(); it != consumers.end(); ++it) {
		/* Cascade this block's output configuration to all connected sinks */
		(*it)->setSampleRate(_outputSampleRate);
		(*it)->setChannels(_outputChannels);
		if (!(*it)->start()) {
			LOG_ERROR("Downstream failed to start - aborting pipeline\n");
			stop();
			return false;
		}
	}

	return true;
}

void DspBlock::stop()
{
	/* Stop downstream first */
	for (vector<DspBlock*>::iterator it = consumers.begin(); it != consumers.end(); ++it)
		(*it)->stop();

	if (_isRunning) {
		LOG_DEBUG("Stopping block %s:%s\n", this->type().c_str(), this->name().c_str());
		_isRunning = false;
		deinit();
	}

	/* Release output buffer */
	vector<sample_t>().swap(buffer);
}

bool DspBlock::run(const vector<sample_t> &inBuffer)
{
	if (!_isRunning) {
		LOG_ERROR("Pipeline not started\n");
		return false;
	}

	/* Resize output buffer */
	unsigned int inframes = inBuffer.size() / _inputChannels;
	unsigned int outframes = inframes * _interpolation / _decimation;
	if (buffer.size() != outframes * _outputChannels) {
		LOG_DEBUG("Resizing %s:%s buffer to %u frames (%u channels)\n",
				this->type().c_str(), this->name().c_str(),
				outframes, _outputChannels);
		buffer.resize(outframes * _outputChannels);
	}

#ifdef DSPBLOCK_PROFILE
	timespec start, end;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
#endif

	/* Process */
	if (!process(inBuffer, buffer)) {
		LOG_ERROR("Pipeline failed at block %s:%s\n", this->type().c_str(), this->name().c_str());
		return false;
	}

#ifdef DSPBLOCK_PROFILE
	/* Profile counters */
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
	_totalNanoseconds += (uint64_t)(end.tv_nsec - start.tv_nsec) +
			(uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ULL;
	_totalIn += inframes;
	_totalOut += outframes;
#endif

	/* Propagate result to all registered consumers */
	for (vector<DspBlock*>::iterator it = consumers.begin(); it != consumers.end(); ++it)
		if (!(*it)->run(buffer))
			return false;

	return true;
}

void DspBlock::setSampleRate(unsigned int rate)
{
	if (_isRunning)
		return;

	LOG_DEBUG("Setting %s:%s input sample rate to %u\n", this->type().c_str(), this->name().c_str(), rate);
	_inputSampleRate = rate;
}

void DspBlock::setChannels(unsigned int channels)
{
	if (_isRunning)
		return;

	LOG_DEBUG("Setting %s:%s input channel count to %u\n", this->type().c_str(), this->name().c_str(), channels);
	_inputChannels = channels;
}

DspSource::DspSource(const string &name, const string &type) : DspBlock(name, type), _blockSize(DEFAULT_BLOCK_SIZE)
{

}

DspSource::~DspSource()
{

}

void DspSource::setBlockSize(unsigned int size)
{
	if (_isRunning)
		return;

	LOG_DEBUG("Setting %s:%s source block size to %u\n", this->type().c_str(), this->name().c_str(), size);
	_blockSize = size;
}
