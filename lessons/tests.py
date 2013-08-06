from django.test import TestCase
from django.core.urlresolvers import reverse
from django.test import LiveServerTestCase
from selenium import webdriver
from selenium.webdriver.chrome.webdriver import WebDriver
from selenium.common.exceptions import NoSuchWindowException, WebDriverException, ErrorInResponseException

from lessons.models import Lesson, LessonFollowsFromLesson
from user_profiles.models import UserProfile, UserTakesLesson, UserAnswersQuestion
from django.contrib.auth.models import User


def create_lesson(nam, tut, des):
    return Lesson.objects.create(name = nam, tutorial = tut, description = des)
    
def create_follows(nam1, nam2, str):
    return LessonFollowsFromLesson.objects.create(leads_from = nam1, leads_to = nam2, strength = str)
        
class MySeleniumTests(LiveServerTestCase):
    fixtures = ['user-data.json']

    @classmethod
    def setUpClass(cls):
        cls.selenium = WebDriver()
        super(MySeleniumTests, cls).setUpClass()

    @classmethod
    def tearDownClass(cls):
        cls.selenium.quit()
        super(MySeleniumTests, cls).tearDownClass()

    def test_user(self):
        print 'Testing user register...'
        try:
            self.selenium.get('%s%s' % (self.live_server_url, '/register/'))
            username_input = self.selenium.find_element_by_name("username")
            username_input.send_keys('TestUser')
            password_1_input = self.selenium.find_element_by_name("password1")
            password_1_input.send_keys('BlaTestUserBla123')
            password_2_input = self.selenium.find_element_by_name("password2")
            password_2_input.send_keys('BlaTestUserBla123')
            self.selenium.find_element_by_xpath('//input[@value="Create the account"]').click()
            print 'SUCESS'
        except:
            print 'FAILURE, testing stoping...'
            return
        print 'Testing log out...'
        try:
            self.selenium.get('%s%s' % (self.live_server_url, '/logout/'))
            print 'SUCESS'
        except:
            print 'FAILURE, testing stoping...'
            return
        print 'Testing log in...'
        try:
            self.selenium.get('%s%s' % (self.live_server_url, '/login/'))
            username_input = self.selenium.find_element_by_name("username")
            username_input.send_keys('TestUser')
            password_input = self.selenium.find_element_by_name("password")
            password_input.send_keys('BlaTestUserBla123')
            self.selenium.find_element_by_xpath('//input[@value="login"]').click()
            print 'SUCESS'
        except:
            print 'FAILURE, testing stoping...'
        print 'Testing complete.'
        
    def test_views(self):
    
        try:
            f = open('lessons/test_unregistered.txt', 'r')
        except:
            print 'Test Failed to load urls file, ending tests.'
            return
            
        urls = list(f)
        
        print 'Checking pages as unregistered user ...'
        
        for url in urls:
            print 'Trying to open ' + url + ' ...'
            try:
                self.selenium.get('%s%s' % (self.live_server_url, url))
                print 'SUCCESS: Loaded ' + url + ' as unregistered user'
            except Exception:
                print 'ERROR: Could not load ' + url + ' page as unregistered user'
        
        f.close()
        
        print 'Done' 
        print 'Trying to registering user ...'
        
        try:
        
            self.selenium.get('%s%s' % (self.live_server_url, '/register/'))
            username_input = self.selenium.find_element_by_name("username")
            username_input.send_keys('TestUser')
            password_1_input = self.selenium.find_element_by_name("password1")
            password_1_input.send_keys('BlaTestUserBla123')
            password_2_input = self.selenium.find_element_by_name("password2")
            password_2_input.send_keys('BlaTestUserBla123')
            self.selenium.find_element_by_xpath('//input[@value="Create the account"]').click()
            print 'SUCESS'
            
        except:
        
            print 'FAILURE, ending test...'
            return
        
        try:
            f = open('lessons/test_unregistered.txt', 'r')
        except:
            print 'Test Failed to load urls file, ending tests.'
            return
        
        urls = list(f)
        
        print 'Checking pages as registered user ...'
        
        for url in urls:
            print 'Trying to open ' + url + ' ...'
            try:
                self.selenium.get('%s%s' % (self.live_server_url, url))
                print 'SUCCESS: Loaded ' + url + ' as registered user'
            except Exception:
                print 'ERROR: Could not load ' + url + ' page as registered user'
        
        f.close()
        print 'Testing complete.'
    
    def test_lessons(self):
        print 'Trying to register...'
        try:
            self.selenium.get('%s%s' % (self.live_server_url, '/register/'))
            username_input = self.selenium.find_element_by_name("username")
            username_input.send_keys('TestUser')
            password_1_input = self.selenium.find_element_by_name("password1")
            password_1_input.send_keys('BlaTestUserBla123')
            password_2_input = self.selenium.find_element_by_name("password2")
            password_2_input.send_keys('BlaTestUserBla123')
            self.selenium.find_element_by_xpath('//input[@value="Create the account"]').click()
            print 'SUCESS'
        except:
            print 'FAILURE, testing stoping...'
            return
        
        print 'Testing lesson creation....'
        try:
            bla1 = create_lesson('bla1', 'bla1', 'bla1')
            bla2 = create_lesson('bla2', 'bla2', 'bla2')
            print 'SUCESS'
        except:
            print 'FAILURE, testing stoping...'
            return
            
        print 'Testing Lesson relation creation....'
        try:
            create_follows(bla1,bla2,10)
            print 'SUCESS'
        except:
            print 'FAILURE, testing stoping...'
            return
            
        print 'Testing Lesson view creation....'
        try:
            self.selenium.get('%s%s' % (self.live_server_url, '/lessons/bla1/'))
            print 'SUCESS'
        except:
            print 'FAILURE, testing stoping...'
            return
            
        print 'Testing Lesson database update...'
        
        print 'View is registered...'
        
        TestUser = User.objects.filter(username = 'TestUser')
        
        if Lesson.objects.filter(usertakeslesson__user = TestUser) == bla1:
            print 'TRUE'
        else:
            print 'FALSE'
        
        print 'Suggested follow up updated...'
        
        if (Lesson.objects.filter(preparation__leads_from__usertakeslesson__user = TestUser).exclude(usertakeslesson__user = TestUser).distinct()[0] == bla2):
            print 'TRUE'
        else:
            print 'FALSE'
            