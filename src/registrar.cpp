//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "registrar.h"

namespace athenasip {

Registrar::Registrar(std::shared_ptr<Logger> logger, std::shared_ptr<mysqlx::Session> session)
    : _logger(std::make_unique<LoggerScoped>("registrar", logger)), _session(session) {}

Registrar::~Registrar() { _session = nullptr; }

bool Registrar::user_exists(const std::string &identity) {
  try {
    // Define the parameterized SQL query
    std::string query = "SELECT `h1`,`h1b` FROM `subscribers` WHERE `identity` = ? AND `realm` = ?";

    // Prepare the statement
    mysqlx::SqlStatement stmt = _session->sql(query);

    // Bind the parameter value
    std::string parameter_value = "desired_value";
    stmt.bind(parameter_value);

    // Execute the query
    mysqlx::SqlResult result = stmt.execute();

    // Iterate over the results
    for (mysqlx::Row row : result) {
      // Process each row as needed
      std::cout << "Column Value: " << row[0] << std::endl;
    }
  } catch (const mysqlx::Error &err) {
    std::cerr << "Error: " << err.what() << std::endl;
    return 1;
  } catch (std::exception &ex) {
    std::cerr << "STD Exception: " << ex.what() << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "Unknown exception occurred." << std::endl;
    return 1;
  }
}

}  // namespace athenasip