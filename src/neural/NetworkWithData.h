





public class NetworkDataclass  : public Network {
public:
  std::unique_ptr<DataClass> AcquireData() {
    std::lock_guard lock(data_mutex_);
    if (network_with_data_.empty()) return MakeData();
    auto result = std::move(network_with_data_.back());
    network_with_data_.pop_back();
    return result;
  }
  void ReleaseData(std::unique_ptr<DataClass> data) {
    std::lock_guard lock(data_mutex_);
    data_pool_.push_back(std::move(data));
  }
  
private:
  virtual std::unique_ptr<DataClass> MakeData() const = 0;
  
  std::mutex data_mutex_;
  std::vector<std::unique_ptr<DataClass>> data_pool_;
};

template <class DataClass>
class NetworkComputationWithData : public NetworkComputation {
  NetworkComputationWithData(NetworkWithData<DataClass>* network)
  : data_(network->AquireData()), network_with_data_(network) {}
  
  ~NetworkComputationWithData() {
    network_with_data_->ReleaseData(std::move(data_));
  }
  
protected:
  DataClass* GetData() { return data_.get(); }
  
private:
  std::unique_ptr<DataClass> data_;
  NetworkWithData<DataClass>* const network_with_data_;
};{
public:
  std::unique_ptr<DataClass> AcquireData() {
    std::lock_guard lock(data_mutex_);
    if (network_with_data_.empty()) return MakeData();
    auto result = std::move(network_with_data_.back());
    network_with_data_.pop_back();
    return result;
  }
  void ReleaseData(std::unique_ptr<DataClass> data) {
    std::lock_guard lock(data_mutex_);
    data_pool_.push_back(std::move(data));
  }
  
private:
  virtual std::unique_ptr<DataClass> MakeData() const = 0;
  
  std::mutex data_mutex_;
  std::vector<std::unique_ptr<DataClass>> data_pool_;
};

template <class DataClass>
class NetworkComputationWithData : public NetworkComputation {
  NetworkComputationWithData(NetworkWithData<DataClass>* network)
  : data_(network->AquireData()), network_with_data_(network) {}
  
  ~NetworkComputationWithData() {
    network_with_data_->ReleaseData(std::move(data_));
  }
  
protected:
  DataClass* GetData() { return data_.get(); }
  
private:
  std::unique_ptr<DataClass> data_;
  NetworkWithData<DataClass>* const network_with_data_;
};
