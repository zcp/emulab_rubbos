/**
 * RUBBoS: Rice University Bulletin Board System.
 * Copyright (C) 2001-2004 Rice University and French National Institute For 
 * Research In Computer Science And Control (INRIA).
 * Contact: jmob@objectweb.org
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or any later
 * version.
 * 
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * Initial developer(s): Emmanuel Cecchet.
 * Contributor(s): Niraj Tolia.
 */

package edu.rice.rubbos.servlets;

import java.io.IOException;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;

import javax.servlet.ServletException;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

//ChienAn
import org.apache.log4j.Logger;

public class Author extends RubbosHttpServlet
{
	//ChienAn
	static Logger responseTimeLogger = Logger.getLogger(Author.class);

  public int getPoolSize()
  {
    return Config.BrowseCategoriesPoolSize;
  }

  private void closeConnection(PreparedStatement stmt, Connection conn)
  {
    try
    {
      if (stmt != null)
        stmt.close(); // close statement
    }
    catch (Exception ignore)
    {
    }

    try
    {
      if (conn != null)
          releaseRConnection(conn);
    }
    catch (Exception ignore)
    {
    }

  }

  /** Build the html page for the response */
  public void doGet(HttpServletRequest request, HttpServletResponse response)
      throws IOException, ServletException
  {
	//ChienAn
	String            logMessage = request.getRequestURI() + "?urlID=" + request.getParameter("urlID");

    ServletPrinter    sp   = null;
    Connection        conn = null;
    PreparedStatement stmt = null;

    sp = new ServletPrinter(response, "Author");

    conn = getRConnection();

    // int storyId =
    // (Integer.valueOf(request.getParameter("storyId"))).intValue();

    /*
     * if (storyId == 0) { sp.printHTML( " <h3> You must provide a story
     * identifier ! <br></h3> "); return; }
     */

    String nickname, password;
    int userId = 0, access = 0;
    ResultSet rs = null;

    nickname = request.getParameter("nickname");
    password = request.getParameter("password");

    if (nickname == null)
    {
      sp.printHTML("Author: You must provide a nick name!<br>");
      closeConnection(stmt, conn);
      return;
    }

    if (password == null)
    {
      sp.printHTML("Author: You must provide a password!<br>");
      closeConnection(stmt, conn);
      return;
    }

    if ((nickname != null) && (password != null))
    {

      try
      {
        stmt = conn
            // Yasu: for PostgreSQL
            // .prepareStatement("SELECT id,access FROM users WHERE nickname=\""
            //     + nickname + "\" AND password=\"" + password + "\"",
            .prepareStatement("SELECT id,access FROM users WHERE nickname='"
//ChienAn                + nickname + "' AND password='" + password + "'",
                + nickname + "' AND password='" + password + "'" + " /* urlID=" + request.getParameter("urlID") + " */",
            // Yasu: FORWARD_ONLY ResultSet doesn't support first() method.
	    ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY);

	//ChienAn
	long startNano = System.nanoTime();

        rs = stmt.executeQuery();

	//ChienAn
        long endNano = System.nanoTime();
        long responseTime = (endNano - startNano)/1000;
        logMessage += " DBST(nano)=" + startNano + " DBET(nano)=" + endNano + " DBRT(us)=" + responseTime;
      }
      catch (Exception e)
      {
        sp.printHTML(" Failed to execute Query for Author: " + e);
        closeConnection(stmt, conn);

	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
                responseTimeLogger.info(logMessage);

        return;
      }
      try
      {
        if (rs.first())
        {
          userId = rs.getInt("id");
          access = rs.getInt("access");
        }
      }
      catch (Exception e)
      {
        sp.printHTML("Exception verifying author: " + e + "<br>");
        closeConnection(stmt, conn);
        /* To make sure there is no double free for the connection */
        conn = null;
        stmt = null;
      }
    }

    closeConnection(stmt, conn);

    if ((userId == 0) || (access == 0))
    {
      sp.printHTMLheader("RUBBoS: Author page");
      sp
          .printHTML("<p><center><h2>Sorry, but this feature is only accessible by users with an author access.</h2></center><p>\n");
    }
    else
    {
      sp.printHTMLheader("RUBBoS: Author page");
      sp
          .printHTML("<p><center><h2>Which administrative task do you want to do ?</h2></center>\n"
              + "<p><p><a href=\"/rubbos/servlet/edu.rice.rubbos.servlets.ReviewStories?authorId= \""
              + userId + "\"\">Review submitted stories</a><br>\n");
    }
    sp.printHTMLfooter();

	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
                responseTimeLogger.info(logMessage);
  }

  public void doPost(HttpServletRequest request, HttpServletResponse response)
      throws IOException, ServletException
  {
    doGet(request, response);
  }

}
