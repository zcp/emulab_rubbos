#!/usr/bin/python

import sys

tomcatHost = "warp38.cc.gatech.edu"
dbServerHost = "warp37.cc.gatech.edu"
workload = 400
remoteClients = ["warp40.cc.gatech.edu","warp41.cc.gatech.edu","warp42.cc.gatech.edu"]

numRemoteClients = len(remoteClients)
numberOfClients = numRemoteClients+1
workloadPerClient = float(workload) / float(numberOfClients)
workloadPerClient = int ( workloadPerClient + 0.9999999 )

#all times are in seconds.  I don't think we'll
#need ms granularity
rampUpTime = 60*2 
runTime = 60*8
downRampTime = 80






print "#n HTTP server information"
print "httpd_hostname = "+tomcatHost
print """httpd_port = 8080

# C/JDBC server to monitor (if any)
cjdbc_hostname =

# Precise which version to use. Valid options are : PHP, Servlets, EJB
httpd_use_version = Servlets

# EJB server information
ejb_server =
ejb_html_path =
ejb_script_path =

# Servlets server information"""
print "servlets_server = "+tomcatHost
print """servlets_html_path = /rubbos
servlets_script_path = /rubbos/servlet

# PHP information
php_html_path = /PHP
php_script_path = /PHP

#Database information"""

print "database_server = "+dbServerHost

print "\n# Workload: precise which transition table to use"
print "workload_remote_client_nodes = ",

for i in range(numRemoteClients):
  sys.stdout.write(remoteClients[i])
  if ( i+1 == numRemoteClients ):
    sys.stdout.write("\n")
  else:
    sys.stdout.write(",")

print """workload_remote_client_command = /tmp/elba/rubbos/jdk1.5.0_07/bin/java -classpath .:/tmp/elba/rubbos/RUBBoS/Client/:/tmp/elba/rubbos/RUBBoS/Client/rubbos_client.jar -Xms128m -Xmx1024m -Dhttp.keepAlive=true -Dhttp.maxConnections=1000000 edu.rice.rubbos.client.ClientEmulator
"""
print "workload_number_of_clients_per_node = "+str(workloadPerClient)

print """
workload_user_transition_table = /tmp/elba/rubbos/RUBBoS/workload/user_transitions.txt
workload_author_transition_table = /tmp/elba/rubbos/RUBBoS/workload/author_transitions.txt
workload_number_of_columns = 24
workload_number_of_rows = 26
workload_maximum_number_of_transitions = 1000
workload_use_tpcw_think_time = yes
workload_number_of_stories_per_page = 20"""

print "workload_up_ramp_time_in_ms = "+str(rampUpTime*1000)
print "workload_up_ramp_slowdown_factor = 2"
print "workload_session_run_time_in_ms = "+str(runTime*1000)
print "workload_down_ramp_time_in_ms = "+str(downRampTime*1000)


print """workload_down_ramp_slowdown_factor = 3
workload_percentage_of_author = 10

# Users policy
database_number_of_authors = 50
database_number_of_users = 500000

# Stories policy
database_story_dictionnary = /tmp/elba/rubbos/RUBBoS/database/dictionary
database_story_maximum_length = 1024
database_oldest_story_year = 1998
database_oldest_story_month = 1

# Comments policy
database_comment_max_length = 1024


# Monitoring Information
monitoring_debug_level = 0
monitoring_program = /tmp/elba/rubbos/sysstat-7.0.0/sar
monitoring_options = -n DEV -n SOCK -rubcw
monitoring_sampling_in_seconds = 1
monitoring_rsh = /usr/bin/ssh
monitoring_scp = /usr/bin/scp
monitoring_gnuplot_terminal = png"""
