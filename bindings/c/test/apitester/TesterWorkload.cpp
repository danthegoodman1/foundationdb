/*
 * TesterWorkload.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "TesterWorkload.h"
#include "TesterUtil.h"
#include <cstdlib>
#include <memory>
#include <fmt/format.h>

namespace FdbApiTester {

int WorkloadConfig::getIntOption(const std::string& name, int defaultVal) const {
	auto iter = options.find(name);
	if (iter == options.end()) {
		return defaultVal;
	} else {
		char* endptr;
		int intVal = strtol(iter->second.c_str(), &endptr, 10);
		if (*endptr != '\0') {
			throw TesterError(
			    fmt::format("Invalid workload configuration. Invalid value {} for {}", iter->second, name));
		}
		return intVal;
	}
}

double WorkloadConfig::getFloatOption(const std::string& name, double defaultVal) const {
	auto iter = options.find(name);
	if (iter == options.end()) {
		return defaultVal;
	} else {
		char* endptr;
		double floatVal = strtod(iter->second.c_str(), &endptr);
		if (*endptr != '\0') {
			throw TesterError(
			    fmt::format("Invalid workload configuration. Invalid value {} for {}", iter->second, name));
		}
		return floatVal;
	}
}

WorkloadBase::WorkloadBase(const WorkloadConfig& config)
  : manager(nullptr), tasksScheduled(0), numErrors(0), clientId(config.clientId), numClients(config.numClients),
    failed(false) {
	maxErrors = config.getIntOption("maxErrors", 10);
	workloadId = fmt::format("{}{}", config.name, clientId);
}

void WorkloadBase::init(WorkloadManager* manager) {
	this->manager = manager;
}

void WorkloadBase::schedule(TTaskFct task) {
	if (failed) {
		return;
	}
	tasksScheduled++;
	manager->scheduler->schedule([this, task]() {
		task();
		tasksScheduled--;
		checkIfDone();
	});
}

void WorkloadBase::execTransaction(std::shared_ptr<ITransactionActor> tx, TTaskFct cont, bool failOnError) {
	if (failed) {
		return;
	}
	tasksScheduled++;
	manager->txExecutor->execute(tx, [this, tx, cont, failOnError]() {
		if (tx->error == error_code_success) {
			cont();
		} else {
			std::string msg =
			    fmt::format("Transaction failed with error: {} ({}})", tx->error, fdb_get_error(tx->error));
			if (failOnError) {
				error(msg);
				failed = true;
			} else {
				info(msg);
				cont();
			}
		}
		tasksScheduled--;
		checkIfDone();
	});
}

void WorkloadBase::info(const std::string& msg) {
	fmt::print(stderr, "[{}] {}\n", workloadId, msg);
}

void WorkloadBase::error(const std::string& msg) {
	fmt::print(stderr, "[{}] ERROR: {}\n", workloadId, msg);
	numErrors++;
	if (numErrors > maxErrors && !failed) {
		fmt::print(stderr, "[{}] ERROR: Stopping workload after {} errors\n", workloadId, numErrors);
		failed = true;
	}
}

void WorkloadBase::checkIfDone() {
	if (tasksScheduled == 0) {
		if (numErrors > 0) {
			error(fmt::format("Workload failed with {} errors", numErrors.load()));
		} else {
			info("Workload successfully completed");
		}
		manager->workloadDone(this, numErrors > 0);
	}
}

void WorkloadManager::add(std::shared_ptr<IWorkload> workload, TTaskFct cont) {
	std::unique_lock<std::mutex> lock(mutex);
	workloads[workload.get()] = WorkloadInfo{ workload, cont };
}

void WorkloadManager::run() {
	for (auto iter : workloads) {
		iter.first->init(this);
	}
	for (auto iter : workloads) {
		iter.first->start();
	}
	scheduler->join();
	if (numWorkloadsFailed > 0) {
		fmt::print(stderr, "{} workloads failed", numWorkloadsFailed);
	} else {
		fprintf(stderr, "All workloads succesfully completed");
	}
}

void WorkloadManager::workloadDone(IWorkload* workload, bool failed) {
	std::unique_lock<std::mutex> lock(mutex);
	auto iter = workloads.find(workload);
	ASSERT(iter != workloads.end());
	lock.unlock();
	iter->second.cont();
	lock.lock();
	workloads.erase(iter);
	if (failed) {
		numWorkloadsFailed++;
	}
	bool done = workloads.empty();
	lock.unlock();
	if (done) {
		scheduler->stop();
	}
}

std::shared_ptr<IWorkload> IWorkloadFactory::create(std::string const& name, const WorkloadConfig& config) {
	auto it = factories().find(name);
	if (it == factories().end())
		return {}; // or throw?
	return it->second->create(config);
}

std::unordered_map<std::string, IWorkloadFactory*>& IWorkloadFactory::factories() {
	static std::unordered_map<std::string, IWorkloadFactory*> theFactories;
	return theFactories;
}

} // namespace FdbApiTester