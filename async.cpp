#include "async.h"

#include <string>
#include <iostream>
#include <list>
#include <algorithm>
#include <chrono>
#include <cassert>
#include <sstream>
#include <set>
#include <atomic>
#include <fstream>
#include <mutex>


namespace async {
//------------------------------------------------------------------------------
/// Для синхронизации для разнообразия решил использовать не мьютекс, а спинлок
class SpinLock
{
  std::atomic_flag locked = ATOMIC_FLAG_INIT;
public:
  /// заблокировать
  void lock() { while ( locked.test_and_set(std::memory_order_acquire) ) { ; } }
  /// разблакировать
  void unlock() { locked.clear(std::memory_order_release); }
};
//------------------------------------------------------------------------------
/// Вывод в стандартный выход
struct MyCout
{
  /// Вывести данные
  MyCout& operator << (const std::string &cmd) {
    std::cout << cmd;
    return *this;
  }
};

/// Вывод в файл
struct MyFile
{
  /// Конструктор
  /// @param time_str время в виде строки
  MyFile(std::string time_str, std::string suffix) {
    std::string name = std::string("bulk-") + suffix +
      std::string("-") + time_str + std::string(".log");
    file.open(name);
  }
  /// Деструктор
  ~MyFile() { file.close(); }
  /// вывести данные
  MyFile& operator << (const std::string &cmd) {
    file << cmd;
    return *this;
  }
private:
  /// Файл
  std::ofstream file;
};

//------------------------------------------------------------------------------
static SpinLock  std_cout_spinlock; ///< для вывода в стандартный выход

/// Команды в блоке
struct BulkCommands
{
  /// Конструктор
  BulkCommands(std::size_t n) : N(n) {}
  /// Деструктор
  ~BulkCommands()
  {
    if ( braces == 0 )
      flush();
  }
  /// добавить команду
  void push(std::string cmd)
  {
    if ( m_commands.empty() ) {
      m_time = std::to_string(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
    }
    m_commands.emplace_back(std::move(cmd));
  }
  /// напечатать все команды в блоке и очистить  
  void flush()
  {
    if ( !m_commands.empty() ) {
      /// выводим в стандартный поток
      MyCout my_cout;
      {
        std::lock_guard<SpinLock> lock(std_cout_spinlock);
        print(my_cout);
      }

      ///  выводим в файл
      std::ostringstream oss;
      oss << (void*)this;
      std::string s(oss.str());
      MyFile myfile(m_time, s);
      {        
        print(myfile);
      }

      m_commands.clear();
    }
  }

  /// напечатать содержимое
  template<typename T>
  void print(T& output)
  {
    output << "    bulk: ";
    for_each(m_commands.begin(), m_commands.end(), [&] (const std::string &cmd) {
      output << cmd << " ";
    });
    std::cout << std::endl;
  }
  /// размер блока
  size_t size() const { return m_commands.size(); }
  /// обработать очередную команду
  void process(const std::string& cmd)
  {
    if ( cmd.find("{") != std::string::npos ) {
      braces++;
      if ( braces == 1 ) {
        flush();
      }
    } else if ( cmd.find("}") != std::string::npos ) {
      braces--;
      if ( braces == 0 ) {
        flush();
      }
    } else {
      push(std::move(cmd));
      if ( braces == 0 && size() == N ) {
        flush();
      }
    }
  }

private:
  const std::size_t N;                 ///< количество команд в блоке
  std::size_t braces{0};               ///< счетчик на скобки
  std::string m_time;                  ///< время первой команды
  std::list<std::string> m_commands;   ///< команды
};

//------------------------------------------------------------------------------
static std::set<BulkCommands*> bulks;
static SpinLock  bulk_spinlock;

handle_t connect(std::size_t bulk_size) 
{
  std::lock_guard<SpinLock> lock(bulk_spinlock);
  BulkCommands * b = new BulkCommands(bulk_size);
  bulks.insert(b);
  return (handle_t)b;
}

void receive(handle_t handle, const char *data, std::size_t size) 
{  
  BulkCommands *commands = nullptr;  
  { 
    std::lock_guard<SpinLock> lock(bulk_spinlock);
    auto it = bulks.find((BulkCommands *) handle);
    assert(it != bulks.end());
    commands = *it;  
  }
  std::string cmd;
  std::istringstream iss(data);  
  while ( iss >> cmd ) {
    commands->process(cmd);
  }  
}

void disconnect(handle_t handle) {
  std::lock_guard<SpinLock> lock(bulk_spinlock);
  BulkCommands *p = (BulkCommands *) handle;
  auto it = bulks.find(p);
  assert(it != bulks.end());
  bulks.erase(it);
  delete p;
}

}
