// ------------------------------------------------------------------------
// eca-plaintext.cpp: Plaintext implementation of the console user 
//                    interface.
// Copyright (C) 2002 Kai Vehmanen (kai.vehmanen@wakkanet.fi)
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
// ------------------------------------------------------------------------

#include <iostream>
#include <string>

#include <eca-version.h>

#include "eca-plaintext.h"

using namespace std;

ECA_PLAIN_TEXT::ECA_PLAIN_TEXT(std::ostream* ostr)
{
  ostream_repp = ostr;
}

ECA_PLAIN_TEXT::~ECA_PLAIN_TEXT(void)
{
}

void ECA_PLAIN_TEXT::print(const std::string& msg)
{
  *ostream_repp << msg << endl;
}

void ECA_PLAIN_TEXT::print_banner(void)
{
  *ostream_repp << "****************************************************************************\n";
  *ostream_repp << "*";
  *ostream_repp << "               ecasound v" 
       << ecasound_library_version
       << " (C) 1997-2002 Kai Vehmanen                 ";
  *ostream_repp << "\n";
  *ostream_repp << "****************************************************************************\n";
}

void ECA_PLAIN_TEXT::read_command(const string& prompt)
{
  *ostream_repp << prompt;
  ostream_repp->flush();
  getline(cin, last_cmd_rep);
}

const string& ECA_PLAIN_TEXT::last_command(void) const
{
  return(last_cmd_rep);
}
