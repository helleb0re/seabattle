#ifdef WIN32
#include <sdkddkver.h>
#endif

#include "seabattle.h"

#include <boost/array.hpp>
#include <boost/asio.hpp>

#include <atomic>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace net = boost::asio;
using net::ip::tcp;
using namespace std::literals;

void PrintFieldPair(const SeabattleField &left, const SeabattleField &right) {
  auto left_pad = "  "s;
  auto delimeter = "    "s;
  std::cout << left_pad;
  SeabattleField::PrintDigitLine(std::cout);
  std::cout << delimeter;
  SeabattleField::PrintDigitLine(std::cout);
  std::cout << std::endl;
  for (size_t i = 0; i < SeabattleField::field_size; ++i) {
    std::cout << left_pad;
    left.PrintLine(std::cout, i);
    std::cout << delimeter;
    right.PrintLine(std::cout, i);
    std::cout << std::endl;
  }
  std::cout << left_pad;
  SeabattleField::PrintDigitLine(std::cout);
  std::cout << delimeter;
  SeabattleField::PrintDigitLine(std::cout);
  std::cout << std::endl;
}

template <size_t sz>
static std::optional<std::string> ReadExact(tcp::socket &socket) {
  boost::array<char, sz> buf;
  boost::system::error_code ec;

  net::read(socket, net::buffer(buf), net::transfer_exactly(sz), ec);

  if (ec) {
    return std::nullopt;
  }

  return {{buf.data(), sz}};
}

static bool WriteExact(tcp::socket &socket, std::string_view data) {
  boost::system::error_code ec;

  net::write(socket, net::buffer(data), net::transfer_exactly(data.size()), ec);

  return !ec;
}

class SeabattleAgent {
public:
  SeabattleAgent(const SeabattleField &field) : my_field_(field) {}

  void StartGame(tcp::socket &socket, bool my_initiative) {
    while (!IsGameEnded()) {
      PrintFields();
      if (my_initiative) {
        std::optional<std::pair<int, int>> move;

        std::string move_str;
        while (true) {
          std::cout << "Your turn: "s;
          std::cin >> move_str;
          move = ParseMove(move_str);

          if (move.has_value()) {
            break;
          }

          std::cout << "This move is incorrect"s << std::endl;
        }

        SendMove(socket, *move);
        my_initiative = ReadResult(socket, *move);
      } else {
        std::cout << "Waiting for turn..."s << std::endl;
        auto [y, x] = ReadMove(socket);
        auto res = my_field_.Shoot(x, y);
        my_initiative = SendResult(socket, res);
      }
    }
  }

private:
  static std::optional<std::pair<int, int>>
  ParseMove(const std::string_view &sv) {
    if (sv.size() != 2)
      return std::nullopt;

    int p1 = sv[0] - 'A', p2 = sv[1] - '1';

    if (p1 < 0 || p1 > 8)
      return std::nullopt;
    if (p2 < 0 || p2 > 8)
      return std::nullopt;

    return {{p1, p2}};
  }

  static std::string MoveToString(std::pair<int, int> move) {
    char buff[] = {static_cast<char>(move.first + 'A'),
                   static_cast<char>(move.second + '1')};
    return {buff, 2};
  }

  void PrintFields() const { PrintFieldPair(my_field_, other_field_); }

  bool IsGameEnded() const {
    return my_field_.IsLoser() || other_field_.IsLoser();
  }

  std::pair<int, int> ReadMove(tcp::socket &socket) const {
    std::optional<std::string> data = ReadExact<2>(socket);
    std::cout << "Shot to " << *data << std::endl;
    return *ParseMove(*data);
  }

  void SendMove(tcp::socket &socket, std::pair<int, int> move) const {
    if (!WriteExact(socket, MoveToString(move))) {
      throw std::runtime_error("Something went wrong..."s);
    }
  }

  bool ReadResult(tcp::socket &socket, std::pair<int, int> move) {
    std::optional<std::string> data = ReadExact<1>(socket);

    int result = std::stoi(*data);
    if (result < 0 || result > 2) {
      throw std::runtime_error("Error result was sent"s);
    }

    auto [y, x] = move;
    switch (SeabattleField::ShotResult{result}) {
    case SeabattleField::ShotResult::MISS: {
      other_field_.MarkMiss(x, y);
      std::cout << "Miss!"s << std::endl;
      return false;
    }
    case SeabattleField::ShotResult::HIT: {
      other_field_.MarkHit(x, y);
      std::cout << "Hit!"s << std::endl;
      return true;
    }
    case SeabattleField::ShotResult::KILL: {
      other_field_.MarkKill(x, y);
      std::cout << "Kill!"s << std::endl;
      return true;
    }
    default:
      break;
    }
  }

  bool SendResult(tcp::socket &socket, SeabattleField::ShotResult shot_result) {
    switch (shot_result) {
    case SeabattleField::ShotResult::MISS: {
      WriteExact(socket, "0"sv);
      return true;
    }
    case SeabattleField::ShotResult::HIT: {
      WriteExact(socket, "1"sv);
      return false;
    }
    case SeabattleField::ShotResult::KILL: {
      WriteExact(socket, "2"sv);
      return false;
    }
    default:
      break;
    }
  }

private:
  SeabattleField my_field_;
  SeabattleField other_field_;
};

void StartServer(const SeabattleField &field, unsigned short port) {
  SeabattleAgent agent(field);

  net::io_context io_context;

  tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), port));
  std::cout << "Waiting for connection..."sv << std::endl;

  boost::system::error_code ec;
  tcp::socket socket{io_context};
  acceptor.accept(socket, ec);

  if (ec) {
    throw std::runtime_error("Can't accept connection"s);
  }

  agent.StartGame(socket, false);
};

void StartClient(const SeabattleField &field, const std::string &ip_str,
                 unsigned short port) {
  SeabattleAgent agent(field);

  boost::system::error_code ec;
  auto endpoint = tcp::endpoint(net::ip::make_address(ip_str, ec), port);

  if (ec) {
    throw std::runtime_error("Wrong IP format"s);
  }

  net::io_context io_context;
  tcp::socket socket{io_context};
  socket.connect(endpoint, ec);

  if (ec) {
    throw std::runtime_error("Can't connect to server"s);
  }

  agent.StartGame(socket, true);
};

int main(int argc, const char **argv) {
  if (argc != 3 && argc != 4) {
    std::cout << "Usage: program <seed> [<ip>] <port>" << std::endl;
    return 1;
  }

  std::mt19937 engine(std::stoi(argv[1]));
  SeabattleField fieldL = SeabattleField::GetRandomField(engine);

  if (argc == 3) {
    StartServer(fieldL, std::stoi(argv[2]));
  } else if (argc == 4) {
    StartClient(fieldL, argv[2], std::stoi(argv[3]));
  }
}
