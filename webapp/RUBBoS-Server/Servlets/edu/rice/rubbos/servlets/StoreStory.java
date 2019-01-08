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
import java.sql.SQLException;

import javax.servlet.ServletException;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

//ChienAn
import org.apache.log4j.Logger;

/**
 * Builds the html page with the list of all categories and provides links to
 * browse all items in a category or items in a category for a given region
 */
public class StoreStory extends RubbosHttpServlet
{
	//ChienAn
	static Logger responseTimeLogger = Logger.getLogger(StoreStory.class);

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
          releaseRWConnection(conn);
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

    ServletPrinter    sp = null;
    PreparedStatement stmt = null;
    Connection        conn = null;

    String categoryName, nickname, title, body, category, table;
    String password = null;
    int userId, access;
    ResultSet rs = null;
    int updateResult;

    sp = new ServletPrinter(response, "StoreStory");

    nickname = request.getParameter("nickname");
    password = request.getParameter("password");
    title = request.getParameter("title");
    body = request.getParameter("body");
    category = request.getParameter("category");

    if (title == null)
    {
      sp.printHTML("You must provide a story title!<br>");
      return;
    }

    if (body == null)
    {
      sp.printHTML("<h3>You must provide a story body!<br></h3>");
      return;
    }

    if (category == null)
    {
      sp.printHTML("<h3>You must provide a category!<br></h3>");
      return;
    }

    sp.printHTMLheader("RUBBoS: Story submission result");

    sp.printHTML("<center><h2>Story submission result:</h2></center><p>\n");

    //Authenticate the user
    userId = 0;
    access = 0;

    conn = getRWConnection();

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
        sp.printHTML("ERROR: Authentification query failed" + e);
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
        stmt.close();
      }
      catch (Exception e)
      {
        sp.printHTML("Exception storing story " + e + "<br>");
        closeConnection(stmt, conn);
        return;
      }
    }

    table = "submissions";
    if (userId == 0)
      sp.printHTML("Story stored by the 'Anonymous Coward'<br>\n");
    else
    {
      if (access == 0)
        sp.printHTML("Story submitted by regular user " + userId + "<br>\n");
      else
      {
        sp.printHTML("Story posted by author " + userId + "<br>\n");
        table = "stories";
      }
    }

    // Add story to database

    try
    {
      // Yasu: for PostgreSQL
      // stmt = conn.prepareStatement("INSERT INTO " + table
      //     + " VALUES (NULL, \"" + title + "\", \"" + body + "\", NOW(), \""
      //     + userId + "\", " + category + ")");
      stmt = conn.prepareStatement("INSERT INTO " + table
          + " (title, body, date, writer, category) VALUES ('"
//ChienAn          + title + "', '" + body + "', NOW(), " + userId + ", " + category + ")");
          + title + "', '" + body + "', NOW(), " + userId + ", " + category + ")" + " /* urlID=" + request.getParameter("urlID") + " */");

	//ChienAn
	long startNano = System.nanoTime();	

      updateResult = stmt.executeUpdate();

	//ChienAn
	long endNano = System.nanoTime();
        long responseTime = (endNano - startNano)/1000;
        logMessage += " DBST(nano)=" + startNano + " DBET(nano)=" + endNano + " DBRT(us)=" + responseTime;	

      if (updateResult != 1)
      {
        sp.printHTML(" ERROR: Failed to insert new story in database. Number of rows updated == " + updateResult +".");
        closeConnection(stmt, conn);
	
	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
                responseTimeLogger.info(logMessage);
        return;
      }
    }
    catch (SQLException e)
    {
      sp.printHTML("Failed to execute Query for StoreStory: " + e);
      closeConnection(stmt, conn);

	//ChienAn
	if (responseTimeLogger.isInfoEnabled())
                responseTimeLogger.info(logMessage);
      return;
    }

    closeConnection(stmt, conn);

    sp.printHTML("Your story has been successfully stored in the " + table
        + " database table<br>\n");

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
